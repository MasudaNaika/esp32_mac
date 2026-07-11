#include "http_server.h"

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_memory_utils.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "mdns.h"

#include "console_shell.h"
#include "console_log.h"
#include "pv_fdd.h"
#include "sdcard.h"
#include "tme/hd.h"
#include "tme/tmeconfig.h"

static constexpr const char *kHostName = "esp32mac";
static constexpr const char *kSetupApSsid = "ESP32Mac-Setup";
static constexpr const char *kSetupApIpString = "192.168.6.1";
static constexpr const char *kWebRoot = "/sd/web";
static constexpr const char *kFdRoot = "/sd/fd";
static constexpr const char *kHdRoot = "/sd/hd";
static constexpr size_t kSendBufferSize = 8192;
static constexpr size_t kTcpMtuBytes = 1454;
static constexpr size_t kHttpWriteSize = 1400;
static constexpr size_t kSdDmaAlignment = 512;
static constexpr size_t kMaxUploadSize = 64u * 1024u * 1024u;

static_assert(kHttpWriteSize < kTcpMtuBytes,
              "HTTP write chunks must stay below the Wi-Fi TCP MTU");

static EventGroupHandle_t sWifiEventGroup;
static constexpr EventBits_t kWifiConnectedBit = BIT0;
static constexpr EventBits_t kWifiFailedBit = BIT1;
static int sWifiRetryCount;
static esp_netif_t *sStaNetif;
static esp_netif_t *sApNetif;
static httpd_handle_t sHttpd;
static char sIpAddress[16] = "";
static AppSettings sSettings;
static char *sFileBuffer;
static bool sWifiInitialized;
static bool sWifiStarted;
static bool sWifiConnected;
static bool sSetupApMode;

typedef struct WifiCandidate {
    char ssid[33];
    char password[65];
} WifiCandidate;

static constexpr int kMaxWifiCandidates = 1 + 10;
static WifiCandidate *sWifiCandidates;

static void logHttpHeap(const char *tag) {
    printf("HTTP heap %s: DRAM free=%u largest=%u DMA free=%u largest=%u PSRAM free=%u largest=%u\n",
           tag,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

static void copyWifiField(uint8_t *dst, size_t dstSize, const char *src) {
    memset(dst, 0, dstSize);
    size_t len = strlen(src);
    if (len >= dstSize) {
        len = dstSize - 1;
    }
    memcpy(dst, src, len);
}

static void copyTextField(char *dst, size_t dstSize, const char *src) {
    if (dstSize == 0) {
        return;
    }
    size_t len = strnlen(src ? src : "", dstSize - 1);
    memcpy(dst, src ? src : "", len);
    dst[len] = '\0';
}

static bool ensureWifiCandidateBuffer(void) {
    if (sWifiCandidates) {
        memset(sWifiCandidates, 0, sizeof(WifiCandidate) * kMaxWifiCandidates);
        return true;
    }
    sWifiCandidates = static_cast<WifiCandidate *>(tme_psram_aligned_calloc(kMaxWifiCandidates,
                                                                            sizeof(WifiCandidate),
                                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!sWifiCandidates) {
        printf("HTTP: failed to allocate Wi-Fi candidate PSRAM buffer\n");
        return false;
    }
    return true;
}

static bool configureSetupApAddress(void) {
    if (!sApNetif) {
        return false;
    }

    esp_err_t err = esp_netif_dhcps_stop(sApNetif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        printf("HTTP: setup AP DHCP stop failed: %s\n", esp_err_to_name(err));
        return false;
    }

    esp_netif_ip_info_t ipInfo = {};
    IP4_ADDR(&ipInfo.ip, 192, 168, 6, 1);
    IP4_ADDR(&ipInfo.gw, 192, 168, 6, 1);
    IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
    err = esp_netif_set_ip_info(sApNetif, &ipInfo);
    if (err != ESP_OK) {
        printf("HTTP: setup AP IP config failed: %s\n", esp_err_to_name(err));
        return false;
    }

    err = esp_netif_dhcps_start(sApNetif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        printf("HTTP: setup AP DHCP start failed: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void wifiEventHandler(void *arg, esp_event_base_t eventBase, int32_t eventId, void *eventData) {
    (void)arg;
    if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_START) {
        printf("HTTP: Wi-Fi event task core %d\n", xPortGetCoreID());
        esp_wifi_connect();
    } else if (eventBase == WIFI_EVENT && eventId == WIFI_EVENT_STA_DISCONNECTED) {
        sWifiConnected = false;
        if (sWifiEventGroup) {
            xEventGroupClearBits(sWifiEventGroup, kWifiConnectedBit);
        }
        if (sWifiRetryCount++ < 8) {
            esp_wifi_connect();
        } else if (sWifiEventGroup) {
            xEventGroupSetBits(sWifiEventGroup, kWifiFailedBit);
            esp_wifi_connect();
        }
    } else if (eventBase == IP_EVENT && eventId == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(eventData);
        snprintf(sIpAddress, sizeof(sIpAddress), IPSTR, IP2STR(&event->ip_info.ip));
        sWifiRetryCount = 0;
        sWifiConnected = true;
        if (sWifiEventGroup) {
            xEventGroupSetBits(sWifiEventGroup, kWifiConnectedBit);
        }
    }
}

static esp_err_t ensureNetworkStack(void) {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    return ESP_OK;
}

static bool ensureWifiInitialized(void) {
    if (sWifiInitialized) {
        return true;
    }
    if (ensureNetworkStack() != ESP_OK) {
        printf("HTTP: network stack init failed\n");
        return false;
    }
    logHttpHeap("before wifi init");

    sWifiEventGroup = xEventGroupCreate();
    if (!sWifiEventGroup) {
        printf("HTTP: could not create Wi-Fi event group\n");
        return false;
    }

    sStaNetif = esp_netif_create_default_wifi_sta();
    if (!sStaNetif) {
        printf("HTTP: could not create Wi-Fi STA netif\n");
        return false;
    }
    esp_netif_set_hostname(sStaNetif, kHostName);

    wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&initCfg);
    if (err != ESP_OK) {
        printf("HTTP: esp_wifi_init failed: %s\n", esp_err_to_name(err));
        return false;
    }
    logHttpHeap("after wifi init");

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifiEventHandler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifiEventHandler, NULL, NULL);
    sWifiInitialized = true;
    return true;
}

static bool connectWifiCandidate(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0') {
        return false;
    }
    if (!ensureWifiInitialized()) {
        return false;
    }

    if (sWifiStarted) {
        esp_wifi_stop();
        sWifiStarted = false;
    }
    sWifiConnected = false;
    sSetupApMode = false;
    sWifiRetryCount = 0;
    sIpAddress[0] = '\0';
    if (sWifiEventGroup) {
        xEventGroupClearBits(sWifiEventGroup, kWifiConnectedBit | kWifiFailedBit);
    }

    wifi_config_t wifiConfig = {};
    copyWifiField(wifiConfig.sta.ssid, sizeof(wifiConfig.sta.ssid), ssid);
    copyWifiField(wifiConfig.sta.password, sizeof(wifiConfig.sta.password), password ? password : "");
    wifiConfig.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifiConfig.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifiConfig));
    ESP_ERROR_CHECK(esp_wifi_start());
    sWifiStarted = true;

    EventBits_t bits = xEventGroupWaitBits(sWifiEventGroup,
                                           kWifiConnectedBit | kWifiFailedBit,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(20000));
    if (!(bits & kWifiConnectedBit)) {
        printf("HTTP: Wi-Fi connect failed or timed out for SSID %s\n", ssid);
        return false;
    }
    printf("HTTP: Wi-Fi connected to %s on core %d, IP %s\n", ssid, xPortGetCoreID(), sIpAddress);
    logHttpHeap("after wifi connect");
    appRememberWifiNetwork(ssid, password ? password : "");
    if (strcmp(sSettings.ssid, ssid) != 0 || strcmp(sSettings.password, password ? password : "") != 0) {
        appSaveTextSetting("ssid", ssid);
        appSaveTextSetting("password", password ? password : "");
        copyTextField(sSettings.ssid, sizeof(sSettings.ssid), ssid);
        copyTextField(sSettings.password, sizeof(sSettings.password), password ? password : "");
    }
    return true;
}

static bool startSetupAccessPoint(void) {
    if (!ensureWifiInitialized()) {
        return false;
    }
    if (sWifiStarted) {
        esp_wifi_stop();
        sWifiStarted = false;
    }
    if (!sApNetif) {
        sApNetif = esp_netif_create_default_wifi_ap();
        if (!sApNetif) {
            printf("HTTP: could not create Wi-Fi AP netif\n");
            return false;
        }
        esp_netif_set_hostname(sApNetif, kHostName);
    }

    wifi_config_t apConfig = {};
    if (!configureSetupApAddress()) {
        return false;
    }

    copyWifiField(apConfig.ap.ssid, sizeof(apConfig.ap.ssid), kSetupApSsid);
    apConfig.ap.ssid_len = strlen(kSetupApSsid);
    apConfig.ap.channel = 1;
    apConfig.ap.max_connection = 2;
    apConfig.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apConfig));
    ESP_ERROR_CHECK(esp_wifi_start());
    sWifiStarted = true;
    sWifiConnected = false;
    sSetupApMode = true;
    snprintf(sIpAddress, sizeof(sIpAddress), "%s", kSetupApIpString);
    printf("HTTP: setup AP ready SSID %s, open http://%s/\n", kSetupApSsid, kSetupApIpString);
    logHttpHeap("after setup ap start");
    return true;
}

static bool connectWifi(const AppSettings *cfg) {
    if (sWifiConnected) {
        return true;
    }
    if (sSetupApMode) {
        return true;
    }
    if (sWifiStarted && sWifiEventGroup) {
        EventBits_t bits = xEventGroupWaitBits(sWifiEventGroup,
                                               kWifiConnectedBit | kWifiFailedBit,
                                               pdFALSE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(20000));
        return (bits & kWifiConnectedBit) != 0;
    }

    if (!ensureWifiCandidateBuffer()) {
        return startSetupAccessPoint();
    }
    int candidateCount = 0;
    if (cfg && cfg->ssid[0] != '\0') {
        copyTextField(sWifiCandidates[candidateCount].ssid, sizeof(sWifiCandidates[candidateCount].ssid), cfg->ssid);
        copyTextField(sWifiCandidates[candidateCount].password, sizeof(sWifiCandidates[candidateCount].password), cfg->password);
        candidateCount++;
    }
    for (int i = 0; candidateCount < kMaxWifiCandidates; ++i) {
        char ssid[33];
        char password[65];
        if (!appLoadWifiListEntry(i, ssid, sizeof(ssid), password, sizeof(password))) {
            break;
        }
        bool duplicate = false;
        for (int j = 0; j < candidateCount; ++j) {
            if (strcmp(sWifiCandidates[j].ssid, ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        copyTextField(sWifiCandidates[candidateCount].ssid, sizeof(sWifiCandidates[candidateCount].ssid), ssid);
        copyTextField(sWifiCandidates[candidateCount].password, sizeof(sWifiCandidates[candidateCount].password), password);
        candidateCount++;
    }
    for (int i = 0; i < candidateCount; ++i) {
        printf("HTTP: trying Wi-Fi SSID %s\n", sWifiCandidates[i].ssid);
        if (connectWifiCandidate(sWifiCandidates[i].ssid, sWifiCandidates[i].password)) {
            return true;
        }
    }

    printf("HTTP: no Wi-Fi station connection; starting setup AP\n");
    return startSetupAccessPoint();
}

static void startMdns(void) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        printf("HTTP: mDNS init failed: %s\n", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(kHostName);
    mdns_instance_name_set("ESP32 Mac Plus");
    mdns_service_add("ESP32 Mac HTTP", "_http", "_tcp", 80, NULL, 0);
    printf("HTTP: mDNS ready at http://%s.local/\n", kHostName);
}

static int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static void urlDecode(char *dst, size_t dstSize, const char *src) {
    if (dstSize == 0) return;
    size_t out = 0;
    for (size_t in = 0; src[in] && out + 1 < dstSize; ++in) {
        if (src[in] == '%' && src[in + 1] && src[in + 2]) {
            int hi = hexValue(src[in + 1]);
            int lo = hexValue(src[in + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = static_cast<char>((hi << 4) | lo);
                in += 2;
                continue;
            }
        }
        dst[out++] = (src[in] == '+') ? ' ' : src[in];
    }
    dst[out] = '\0';
}

static const char *contentTypeForPath(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css") == 0) return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js") == 0) return "application/javascript; charset=utf-8";
    if (strcasecmp(ext, ".json") == 0) return "application/json; charset=utf-8";
    if (strcasecmp(ext, ".png") == 0) return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcasecmp(ext, ".txt") == 0) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static bool hasBadPathSegment(const char *path) {
    return strstr(path, "..") || strchr(path, '\\') || strchr(path, ':');
}

static bool uriPathEquals(const char *uri, const char *path) {
    size_t len = strlen(path);
    return strncmp(uri, path, len) == 0 && (uri[len] == '\0' || uri[len] == '?');
}

static bool mapWebPath(const char *uri, char *path, size_t pathSize) {
    const char *name = uri;
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        name = "index.html";
    } else if (strcmp(uri, "/console") == 0) {
        name = "console.html";
    } else if (strcmp(uri, "/setting") == 0) {
        name = "setting.html";
    } else if (strcmp(uri, "/files") == 0) {
        name = "files.html";
    } else if (uri[0] == '/') {
        name = uri + 1;
    }
    if (!name[0] || hasBadPathSegment(name)) {
        return false;
    }
    if (strlen(kWebRoot) + 1 + strlen(name) >= pathSize) {
        return false;
    }
    snprintf(path, pathSize, "%s/%s", kWebRoot, name);
    return true;
}

static bool mapUploadPath(const char *requested, char *path, size_t pathSize) {
    char decoded[160];
    urlDecode(decoded, sizeof(decoded), requested);
    char *name = decoded;
    while (*name == '/') name++;
    if (strncmp(name, "sd/", 3) == 0) {
        name += 3;
        size_t len = strlen(name);
        if (!name[0] || name[len - 1] == '/' || hasBadPathSegment(name) || strstr(name, "//") ||
            strlen("/sd/") + len >= pathSize) {
            return false;
        }
        snprintf(path, pathSize, "/sd/%s", name);
        return true;
    }
    if (strncmp(name, "web/", 4) == 0) {
        name += 4;
        if (!name[0] || hasBadPathSegment(name)) {
            return false;
        }
        snprintf(path, pathSize, "%s/%s", kWebRoot, name);
        return true;
    }
    if (strncmp(name, "fd/", 3) == 0) {
        name += 3;
        if (!name[0] || hasBadPathSegment(name)) {
            return false;
        }
        snprintf(path, pathSize, "%s/%s", kFdRoot, name);
        return true;
    }
    if (strncmp(name, "hd/", 3) == 0) {
        name += 3;
        if (!name[0] || hasBadPathSegment(name)) {
            return false;
        }
        snprintf(path, pathSize, "%s/%s", kHdRoot, name);
        return true;
    }
    if (!name[0] || hasBadPathSegment(name)) {
        return false;
    }
    if (strchr(name, '/') || strlen("/sd/") + strlen(name) >= pathSize) {
        return false;
    }
    snprintf(path, pathSize, "/sd/%s", name);
    return true;
}

static esp_err_t sendFile(httpd_req_t *req, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }
    httpd_resp_set_type(req, contentTypeForPath(path));
    httpd_resp_set_hdr(req, "Connection", "close");

    if (!sFileBuffer) {
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no DMA buffer");
        return ESP_FAIL;
    }

    const int64_t transferStartUs = esp_timer_get_time();
    size_t totalBytes = 0;
    size_t read = 0;
    esp_err_t ret = ESP_OK;
    printf("HTTP: file send start %s read_buf=%u chunk=%u dram=%u/%u dma=%u/%u core=%d\n",
           path,
           static_cast<unsigned>(kSendBufferSize),
           static_cast<unsigned>(kHttpWriteSize),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
           xPortGetCoreID());
    while ((read = fread(sFileBuffer, 1, kSendBufferSize, fp)) > 0) {
        const size_t blockOffset = totalBytes;
        const int64_t blockStartUs = esp_timer_get_time();
        size_t sentBytes = 0;
        while (sentBytes < read) {
            const size_t sendLen = (read - sentBytes > kHttpWriteSize) ? kHttpWriteSize : (read - sentBytes);
            const int64_t startUs = esp_timer_get_time();
            esp_err_t sendErr = httpd_resp_send_chunk(req, sFileBuffer + sentBytes, sendLen);
            const int64_t elapsedMs = (esp_timer_get_time() - startUs) / 1000;
            if (sendErr == ESP_OK) {
                sentBytes += sendLen;
                totalBytes += sendLen;
                continue;
            }
            printf("HTTP: file send failed %s offset=%u bytes=%u err=%s elapsed=%lldms\n",
                   path,
                   static_cast<unsigned>(sentBytes),
                   static_cast<unsigned>(sendLen),
                   esp_err_to_name(sendErr),
                   static_cast<long long>(elapsedMs));
            ret = ESP_FAIL;
            break;
        }
        if (ret != ESP_OK) {
            break;
        }
        const int64_t blockElapsedMs = (esp_timer_get_time() - blockStartUs) / 1000;
        printf("HTTP: file block sent %s offset=%u bytes=%u elapsed=%lldms total=%u\n",
               path,
               static_cast<unsigned>(blockOffset),
               static_cast<unsigned>(read),
               static_cast<long long>(blockElapsedMs),
               static_cast<unsigned>(totalBytes));
        taskYIELD();
    }
    fclose(fp);
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
        const int64_t elapsedMs = (esp_timer_get_time() - transferStartUs) / 1000;
        printf("HTTP: file sent %s bytes=%u elapsed=%lldms core=%d\n",
               path,
               static_cast<unsigned>(totalBytes),
               static_cast<long long>(elapsedMs),
               xPortGetCoreID());
    } else {
        int sockfd = httpd_req_to_sockfd(req);
        if (sockfd >= 0 && sHttpd) {
            httpd_sess_trigger_close(sHttpd, sockfd);
        }
    }
    return ret;
}

static const char kBootstrapIndexHtml[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<meta name=\"esp32-mac-bootstrap\" content=\"1\">"
    "<title>ESP32 Mac Setup</title>"
    "<style>body{font-family:system-ui,sans-serif;margin:24px;line-height:1.45}"
    "label{display:block;margin:12px 0 4px}input,select,button{font:inherit;padding:8px;width:100%;box-sizing:border-box}"
    "button{margin-top:12px}pre{white-space:pre-wrap;background:#111;color:#ddd;padding:12px;min-height:48px}</style>"
    "</head><body><h1>ESP32 Mac Setup</h1>"
    "<p>Web UI files are missing. Choose a file, then choose where to save it on the ESP32.</p>"
    "<label>Save folder on ESP32</label><select id=\"folder\">"
    "<option value=\"\" selected>/</option>"
    "<option value=\"web\">/web</option>"
    "<option value=\"fd\">/fd</option>"
    "<option value=\"hd\">/hd</option>"
    "</select>"
    "<label>File to upload</label><input id=\"file\" type=\"file\">"
    "<button id=\"upload\">Upload</button><pre id=\"status\"></pre>"
    "<script>const BOOT='name=\"esp32-mac-bootstrap\"';const $=id=>document.getElementById(id);"
    "const clean=n=>(n||'').replace(/^.*[\\\\/]/,'');"
    "function targetPath(f){const d=$('folder').value,n=clean(f&&f.name);return d?d+'/'+n:n}"
    "async function openIndexIfReady(){try{const r=await fetch('/index.html?check='+Date.now(),{cache:'no-store'});"
    "if(!r.ok)return;const t=await r.text();if(!t.includes(BOOT))location.replace('/')}catch(e){}}"
    "setInterval(openIndexIfReady,3000);$('upload').onclick=async()=>{"
    "const f=$('file').files[0];if(!f){$('status').textContent='select file';return}"
    "const p=targetPath(f);if(!p){$('status').textContent='select file';return}"
    "$('status').textContent='uploading '+p+' ...';"
    "try{const r=await fetch('/upload?path='+encodeURIComponent(p),{method:'PUT',body:f});"
    "$('status').textContent=await r.text();if(r.ok){if(p.replace(/^\\/+/, '').toLowerCase()==='web/index.html')"
    "setTimeout(()=>location.replace('/'),500);else openIndexIfReady()}}catch(e){$('status').textContent=e}}</script>"
    "</body></html>";

static esp_err_t getHandler(httpd_req_t *req) {
    if (strcmp(req->uri, "/favicon.ico") == 0) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }

    char path[192];
    if (!mapWebPath(req->uri, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    if (sendFile(req, path) == ESP_OK) {
        return ESP_OK;
    }
    if (strcmp(req->uri, "/") == 0 || strcmp(req->uri, "/index.html") == 0) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, kBootstrapIndexHtml, HTTPD_RESP_USE_STRLEN);
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("HTTP: not found %s -> %s\n", req->uri, path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    } else {
        fclose(fp);
        printf("HTTP: file send aborted %s -> %s\n", req->uri, path);
    }
    return ESP_OK;
}

static bool queryPath(httpd_req_t *req, char *value, size_t valueSize) {
    char query[192];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, "path", value, valueSize) == ESP_OK ||
           httpd_query_key_value(query, "file", value, valueSize) == ESP_OK ||
           httpd_query_key_value(query, "name", value, valueSize) == ESP_OK;
}

static int readRequestBody(httpd_req_t *req, char *body, size_t bodySize) {
    if (!body || bodySize == 0 || req->content_len >= bodySize) {
        return -1;
    }
    size_t remaining = req->content_len;
    int offset = 0;
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + offset, static_cast<int>(remaining));
        if (received <= 0) {
            return -1;
        }
        remaining -= static_cast<size_t>(received);
        offset += received;
    }
    body[offset] = '\0';
    return offset;
}

static bool formValue(const char *body, const char *key, char *value, size_t valueSize) {
    size_t keyLen = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
            const char *start = p + keyLen + 1;
            const char *end = strchr(start, '&');
            char encoded[160];
            size_t len = end ? static_cast<size_t>(end - start) : strlen(start);
            if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
            memcpy(encoded, start, len);
            encoded[len] = '\0';
            urlDecode(value, valueSize, encoded);
            return true;
        }
        p = strchr(p, '&');
        if (p) ++p;
    }
    return false;
}

static void appendText(char *dst, size_t dstSize, const char *text);
static char *allocPsramTextBuffer(size_t size, const char *tag);

static esp_err_t consoleApiHandler(httpd_req_t *req) {
    char body[128];
    if (readRequestBody(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad command");
        return ESP_FAIL;
    }
    constexpr size_t kConsoleResponseSize = 2048;
    char *response = allocPsramTextBuffer(kConsoleResponseSize, "console response");
    if (!response) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_FAIL;
    }
    bool ok = consoleShellRunCommand(body, response, kConsoleResponseSize);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, ok ? "200 OK" : "400 Bad Request");
    esp_err_t sendErr = httpd_resp_sendstr(req, response[0] ? response : (ok ? "OK\n" : "ERROR\n"));
    heap_caps_free(response);
    if (sendErr != ESP_OK) {
        return ESP_FAIL;
    }
    return ok ? ESP_OK : ESP_FAIL;
}

static uint32_t consoleEventCursor(httpd_req_t *req) {
    char lastId[16];
    if (httpd_req_get_hdr_value_str(req, "Last-Event-ID", lastId, sizeof(lastId)) == ESP_OK) {
        char *end = NULL;
        unsigned long parsed = strtoul(lastId, &end, 10);
        if (end != lastId && *end == '\0') {
            return (uint32_t)parsed;
        }
    }
    return consoleLogWriteSeq();
}

static esp_err_t sendSseLogEvent(httpd_req_t *req, uint32_t id, const char *text, size_t len) {
    char header[40];
    snprintf(header, sizeof(header), "id: %lu\n", (unsigned long)id);
    esp_err_t err = httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req, "data: ", HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < len; ++i) {
        char ch = text[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            err = httpd_resp_send_chunk(req, "\ndata: ", HTTPD_RESP_USE_STRLEN);
        } else {
            err = httpd_resp_send_chunk(req, &ch, 1);
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_resp_send_chunk(req, "\n\n", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t consoleEventsHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/event-stream; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "X-Accel-Buffering", "no");

    uint32_t cursor = consoleEventCursor(req);
    char chunk[512];
    size_t len = consoleLogRead(&cursor, chunk, sizeof(chunk));
    if (len > 0) {
        if (sendSseLogEvent(req, cursor, chunk, len) != ESP_OK) {
            return ESP_FAIL;
        }
    } else {
        char retry[48];
        snprintf(retry, sizeof(retry), "retry: 500\nid: %lu\n\n", (unsigned long)cursor);
        if (httpd_resp_send_chunk(req, retry, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static void appendJsonEscaped(char *dst, size_t dstSize, const char *key, const char *value, bool *first) {
    size_t used = strlen(dst);
    if (used >= dstSize - 1) {
        return;
    }
    int written = snprintf(dst + used, dstSize - used, "%s\"%s\":\"", *first ? "" : ",", key);
    if (written < 0 || static_cast<size_t>(written) >= dstSize - used) {
        return;
    }
    used += static_cast<size_t>(written);
    for (const char *p = value ? value : ""; *p && used + 3 < dstSize; ++p) {
        if (*p == '"' || *p == '\\') {
            dst[used++] = '\\';
        }
        dst[used++] = *p;
    }
    if (used + 2 < dstSize) {
        dst[used++] = '"';
        dst[used] = '\0';
    }
    *first = false;
}

static void appendJsonString(char *dst, size_t dstSize, const char *value) {
    size_t used = strlen(dst);
    if (used >= dstSize - 1) {
        return;
    }
    dst[used++] = '"';
    for (const char *p = value ? value : ""; *p && used + 3 < dstSize; ++p) {
        if (*p == '"' || *p == '\\') {
            dst[used++] = '\\';
        }
        if (static_cast<unsigned char>(*p) >= 0x20) {
            dst[used++] = *p;
        }
    }
    if (used < dstSize - 1) {
        dst[used++] = '"';
        dst[used] = '\0';
    }
}

static void appendText(char *dst, size_t dstSize, const char *text) {
    if (!dst || dstSize == 0 || !text) {
        return;
    }
    strlcat(dst, text, dstSize);
}

static char *allocPsramTextBuffer(size_t size, const char *tag) {
    char *buf = static_cast<char *>(tme_psram_aligned_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        printf("HTTP: failed to allocate PSRAM buffer for %s (%u bytes)\n",
               tag,
               static_cast<unsigned>(size));
    }
    return buf;
}

static wifi_ap_record_t *allocWifiScanRecords(size_t count, const char *tag) {
    wifi_ap_record_t *records = static_cast<wifi_ap_record_t *>(tme_psram_aligned_calloc(count,
                                                                                         sizeof(wifi_ap_record_t),
                                                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!records) {
        printf("HTTP: failed to allocate PSRAM Wi-Fi scan records for %s (%u entries)\n",
               tag,
               static_cast<unsigned>(count));
    }
    return records;
}

static bool hasExtension(const char *name, const char *const *extensions) {
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return false;
    }
    for (int i = 0; extensions[i]; ++i) {
        if (strcasecmp(ext, extensions[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool mountedStoragePathMatches(const char *absPath, bool isDir);

static esp_err_t sendJsonQuotedChunk(httpd_req_t *req, const char *value) {
    esp_err_t err = httpd_resp_send_chunk(req, "\"", HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    for (const char *p = value ? value : ""; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            char escaped[2] = {'\\', *p};
            err = httpd_resp_send_chunk(req, escaped, sizeof(escaped));
        } else if (static_cast<unsigned char>(*p) >= 0x20) {
            err = httpd_resp_send_chunk(req, p, 1);
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_resp_send_chunk(req, "\"", HTTPD_RESP_USE_STRLEN);
}

static bool mapSdBrowserPath(const char *requested,
                             char *absolute,
                             size_t absoluteSize,
                             char *relative,
                             size_t relativeSize,
                             bool allowRoot) {
    char decoded[160];
    urlDecode(decoded, sizeof(decoded), requested ? requested : "");
    char *name = decoded;
    while (*name == '/') {
        ++name;
    }
    if (strncmp(name, "sd/", 3) == 0) {
        name += 3;
    } else if (strcmp(name, "sd") == 0) {
        name += 2;
        while (*name == '/') {
            ++name;
        }
    }
    size_t len = strlen(name);
    while (len > 0 && name[len - 1] == '/') {
        name[--len] = '\0';
    }
    if ((!allowRoot && len == 0) || strcmp(name, ".") == 0 ||
        hasBadPathSegment(name) || strstr(name, "//")) {
        return false;
    }
    if (len >= relativeSize || strlen("/sd") + 1 + len >= absoluteSize) {
        return false;
    }
    strlcpy(relative, name, relativeSize);
    if (len == 0) {
        snprintf(absolute, absoluteSize, "/sd");
    } else {
        snprintf(absolute, absoluteSize, "/sd/%s", name);
    }
    return true;
}

static bool queryBrowserPath(httpd_req_t *req, char *value, size_t valueSize) {
    char query[192];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", value, valueSize) != ESP_OK) {
        strlcpy(value, "", valueSize);
    }
    return true;
}

static void parentBrowserPath(const char *path, char *parent, size_t parentSize) {
    strlcpy(parent, path ? path : "", parentSize);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
    } else if (parentSize > 0) {
        parent[0] = '\0';
    }
}

static esp_err_t sendFilerEntry(httpd_req_t *req,
                                const char *dirAbs,
                                const char *dirRel,
                                const char *name,
                                bool *first) {
    char childAbs[512];
    char childRel[512];
    snprintf(childAbs, sizeof(childAbs), "%s/%s", dirAbs, name);
    snprintf(childRel, sizeof(childRel), "%s%s%s", dirRel, dirRel[0] ? "/" : "", name);

    struct stat st;
    if (stat(childAbs, &st) != 0) {
        return ESP_OK;
    }
    if (!*first) {
        esp_err_t err = httpd_resp_send_chunk(req, ",", HTTPD_RESP_USE_STRLEN);
        if (err != ESP_OK) {
            return err;
        }
    }
    *first = false;

    esp_err_t err = httpd_resp_send_chunk(req, "{\"name\":", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = sendJsonQuotedChunk(req, name);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, ",\"path\":", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = sendJsonQuotedChunk(req, childRel);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, ",\"type\":", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = sendJsonQuotedChunk(req, S_ISDIR(st.st_mode) ? "dir" : "file");
    if (err == ESP_OK) {
        char sizeText[64];
        snprintf(sizeText,
                 sizeof(sizeText),
                 ",\"size\":%lld,\"mounted\":%s}",
                 (long long)st.st_size,
                 mountedStoragePathMatches(childAbs, S_ISDIR(st.st_mode)) ? "true" : "false");
        err = httpd_resp_send_chunk(req, sizeText, HTTPD_RESP_USE_STRLEN);
    }
    return err;
}

static esp_err_t filerListHandler(httpd_req_t *req) {
    char requested[160];
    queryBrowserPath(req, requested, sizeof(requested));
    char absPath[192];
    char relPath[160];
    if (!mapSdBrowserPath(requested, absPath, sizeof(absPath), relPath, sizeof(relPath), true)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    DIR *dir = opendir(absPath);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    char parent[160];
    parentBrowserPath(relPath, parent, sizeof(parent));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t err = httpd_resp_send_chunk(req, "{\"path\":", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = sendJsonQuotedChunk(req, relPath);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, ",\"parent\":", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = sendJsonQuotedChunk(req, parent);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, ",\"entries\":[", HTTPD_RESP_USE_STRLEN);

    bool first = true;
    while (err == ESP_OK) {
        dirent *entry = readdir(dir);
        if (!entry) {
            break;
        }
        if (entry->d_name[0] == '.' || hasBadPathSegment(entry->d_name)) {
            continue;
        }
        err = sendFilerEntry(req, absPath, relPath, entry->d_name, &first);
    }
    closedir(dir);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t filerDownloadHandler(httpd_req_t *req) {
    char requested[160];
    queryBrowserPath(req, requested, sizeof(requested));
    char absPath[192];
    char relPath[160];
    if (!mapSdBrowserPath(requested, absPath, sizeof(absPath), relPath, sizeof(relPath), false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    struct stat st;
    if (stat(absPath, &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    const char *name = strrchr(relPath, '/');
    name = name ? name + 1 : relPath;
    char disposition[256];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    return sendFile(req, absPath);
}

static bool requestFormPath(httpd_req_t *req,
                            char *absolute,
                            size_t absoluteSize,
                            char *relative,
                            size_t relativeSize,
                            bool allowRoot) {
    char body[192];
    char requested[160];
    return readRequestBody(req, body, sizeof(body)) >= 0 &&
           formValue(body, "path", requested, sizeof(requested)) &&
           mapSdBrowserPath(requested, absolute, absoluteSize, relative, relativeSize, allowRoot);
}

static bool pathIsSelfOrParentOf(const char *candidate, const char *mountedPath, bool candidateIsDir) {
    if (!candidate || !mountedPath || !candidate[0] || !mountedPath[0]) {
        return false;
    }
    size_t candidateLen = strlen(candidate);
    if (strcmp(candidate, mountedPath) == 0) {
        return true;
    }
    return candidateIsDir &&
           candidateLen > 0 &&
           strncmp(candidate, mountedPath, candidateLen) == 0 &&
           mountedPath[candidateLen] == '/';
}

static bool mountedStoragePathMatches(const char *absPath, bool isDir) {
    char mounted[192];
    char resolved[192];
    for (uint8_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
        mounted[0] = '\0';
        pvFddGetMountedName(drive, mounted, sizeof(mounted));
        if (!mounted[0]) {
            continue;
        }
        const char *path = resolveSdConfigPath(mounted, nullptr, resolved, sizeof(resolved));
        if (path && pathIsSelfOrParentOf(absPath, path, isDir)) {
            return true;
        }
    }
    for (int target = 0; target < SCSI_TARGET_COUNT; ++target) {
        mounted[0] = '\0';
        hdGetMountedPath(target, mounted, sizeof(mounted));
        if (mounted[0] && pathIsSelfOrParentOf(absPath, mounted, isDir)) {
            return true;
        }
    }
    return false;
}

static esp_err_t filerMkdirHandler(httpd_req_t *req) {
    char absPath[192];
    char relPath[160];
    if (!requestFormPath(req, absPath, sizeof(absPath), relPath, sizeof(relPath), false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    if (mkdir(absPath, 0775) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t filerDeleteHandler(httpd_req_t *req) {
    char absPath[192];
    char relPath[160];
    if (!requestFormPath(req, absPath, sizeof(absPath), relPath, sizeof(relPath), false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    struct stat st;
    if (stat(absPath, &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    if (mountedStoragePathMatches(absPath, S_ISDIR(st.st_mode))) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "mounted image is in use");
        return ESP_FAIL;
    }
    int rc = S_ISDIR(st.st_mode) ? rmdir(absPath) : remove(absPath);
    if (rc != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            S_ISDIR(st.st_mode) ? "rmdir failed" : "delete failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t filerRenameHandler(httpd_req_t *req) {
    char body[256];
    char requested[160];
    char newName[96];
    char oldAbs[192];
    char oldRel[160];
    if (readRequestBody(req, body, sizeof(body)) < 0 ||
        !formValue(body, "path", requested, sizeof(requested)) ||
        !formValue(body, "name", newName, sizeof(newName)) ||
        !mapSdBrowserPath(requested, oldAbs, sizeof(oldAbs), oldRel, sizeof(oldRel), false) ||
        !newName[0] || strcmp(newName, ".") == 0 ||
        hasBadPathSegment(newName) || strchr(newName, '/')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    struct stat oldSt;
    if (stat(oldAbs, &oldSt) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    if (mountedStoragePathMatches(oldAbs, S_ISDIR(oldSt.st_mode))) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "mounted image is in use");
        return ESP_FAIL;
    }

    char parentRel[160];
    char newRel[320];
    char newAbs[512];
    parentBrowserPath(oldRel, parentRel, sizeof(parentRel));
    snprintf(newRel, sizeof(newRel), "%s%s%s",
             parentRel,
             parentRel[0] ? "/" : "",
             newName);
    if (!mapSdBrowserPath(newRel, newAbs, sizeof(newAbs), newRel, sizeof(newRel), false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    struct stat newSt;
    if (stat(newAbs, &newSt) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target exists");
        return ESP_FAIL;
    }
    if (rename(oldAbs, newAbs) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t sendFileOptionsArray(httpd_req_t *req,
                                      const char *jsonKey,
                                      const char *directory,
                                      const char *valuePrefix,
                                      const char *const *extensions,
                                      bool *firstSection) {
    esp_err_t err = httpd_resp_send_chunk(req, *firstSection ? "\"" : ",\"", HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req, jsonKey, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_resp_send_chunk(req, "\":[", HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }
    *firstSection = false;

    DIR *dir = opendir(directory);
    bool firstItem = true;
    if (dir) {
        while (dirent *entry = readdir(dir)) {
            if (entry->d_name[0] == '.' || entry->d_type == DT_DIR ||
                hasBadPathSegment(entry->d_name) || !hasExtension(entry->d_name, extensions)) {
                continue;
            }
            if (!firstItem) {
                err = httpd_resp_send_chunk(req, ",", HTTPD_RESP_USE_STRLEN);
                if (err != ESP_OK) {
                    closedir(dir);
                    return err;
                }
            }
            firstItem = false;
            char value[320];
            snprintf(value, sizeof(value), "%s%s", valuePrefix, entry->d_name);
            err = sendJsonQuotedChunk(req, value);
            if (err != ESP_OK) {
                closedir(dir);
                return err;
            }
        }
        closedir(dir);
    }
    return httpd_resp_send_chunk(req, "]", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t fileOptionsHandler(httpd_req_t *req) {
    static const char *const romExtensions[] = { ".rom", NULL };
    static const char *const fdExtensions[] = { ".dsk", ".img", NULL };
    static const char *const hdExtensions[] = { ".img", ".dsk", ".hd", NULL };

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t err = httpd_resp_send_chunk(req, "{", HTTPD_RESP_USE_STRLEN);
    bool firstSection = true;
    if (err == ESP_OK) {
        err = sendFileOptionsArray(req, "rom", "/sd", "", romExtensions, &firstSection);
    }
    if (err == ESP_OK) {
        err = sendFileOptionsArray(req, "fd", kFdRoot, "fd/", fdExtensions, &firstSection);
    }
    if (err == ESP_OK) {
        err = sendFileOptionsArray(req, "hd", kHdRoot, "hd/", hdExtensions, &firstSection);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "}", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t settingsGetHandler(httpd_req_t *req) {
    constexpr size_t kSettingsJsonSize = 4096;
    char *json = allocPsramTextBuffer(kSettingsJsonSize, "settings json");
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_FAIL;
    }
    snprintf(json, kSettingsJsonSize, "{");
    bool first = true;
    appendJsonEscaped(json, kSettingsJsonSize, "ssid", sSettings.ssid, &first);
    appendJsonEscaped(json, kSettingsJsonSize, "password", sSettings.password, &first);
    appendJsonEscaped(json, kSettingsJsonSize, "server", sSettings.server, &first);
    char tz[24];
    int32_t tzSeconds = sSettings.tzOffsetSeconds;
    char sign = tzSeconds < 0 ? '-' : '+';
    int32_t absSeconds = tzSeconds < 0 ? -tzSeconds : tzSeconds;
    snprintf(tz, sizeof(tz), "%c%02ld:%02ld",
             sign,
             (long)(absSeconds / 3600),
             (long)((absSeconds % 3600) / 60));
    appendJsonEscaped(json, kSettingsJsonSize, "tz", tz, &first);
    appendJsonEscaped(json, kSettingsJsonSize, "rom", sSettings.rom, &first);
    appendJsonEscaped(json, kSettingsJsonSize, "fd0", sSettings.fd[0], &first);
    appendJsonEscaped(json, kSettingsJsonSize, "fd1", sSettings.fd[1], &first);
    for (int id = 0; id < SCSI_TARGET_COUNT; ++id) {
        char key[4];
        snprintf(key, sizeof(key), "hd%d", id);
        appendJsonEscaped(json, kSettingsJsonSize, key, sSettings.hd[id], &first);
    }
    appendJsonEscaped(json, kSettingsJsonSize, "log", sSettings.consoleOut ? "on" : "off", &first);
    appendJsonEscaped(json, kSettingsJsonSize, "lcdflip", sSettings.displayRotate180 ? "on" : "off", &first);
    appendJsonEscaped(json, kSettingsJsonSize, "turbo", sSettings.turbo ? "on" : "off", &first);
    appendJsonEscaped(json, kSettingsJsonSize, "audio", appAudioBackendName(sSettings.audioBackend), &first);
    appendJsonEscaped(json, kSettingsJsonSize, "boot_info", sSettings.bootInfo ? "on" : "off", &first);
    strlcat(json, "}", kSettingsJsonSize);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t sendErr = httpd_resp_sendstr(req, json);
    heap_caps_free(json);
    return sendErr == ESP_OK ? ESP_OK : ESP_FAIL;
}

static const char *authModeName(wifi_auth_mode_t authMode) {
    switch (authMode) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-enterprise";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    case WIFI_AUTH_WAPI_PSK: return "wapi";
    default: return "unknown";
    }
}

static esp_err_t wifiScanHandler(httpd_req_t *req) {
    wifi_scan_config_t scanConfig = {};
    esp_err_t err = esp_wifi_scan_start(&scanConfig, true);
    if (err != ESP_OK) {
        printf("HTTP: Wi-Fi scan failed: %s\n", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    uint16_t apCount = 0;
    esp_wifi_scan_get_ap_num(&apCount);
    if (apCount > 20) {
        apCount = 20;
    }

    wifi_ap_record_t *records = allocWifiScanRecords(apCount, "api wifi scan");
    if (!records) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_FAIL;
    }
    uint16_t recordCount = apCount;
    err = esp_wifi_scan_get_ap_records(&recordCount, records);
    if (err != ESP_OK) {
        printf("HTTP: Wi-Fi scan records failed: %s\n", esp_err_to_name(err));
        heap_caps_free(records);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    constexpr size_t kWifiScanJsonSize = 3072;
    char *json = allocPsramTextBuffer(kWifiScanJsonSize, "wifi scan json");
    if (!json) {
        heap_caps_free(records);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_FAIL;
    }
    snprintf(json, kWifiScanJsonSize, "{\"networks\":[");
    bool first = true;
    for (uint16_t i = 0; i < recordCount; ++i) {
        char ssid[33];
        memcpy(ssid, records[i].ssid, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
        if (ssid[0] == '\0') {
            continue;
        }
        strlcat(json, first ? "{" : ",{", kWifiScanJsonSize);
        strlcat(json, "\"ssid\":", kWifiScanJsonSize);
        appendJsonString(json, kWifiScanJsonSize, ssid);
        char detail[96];
        snprintf(detail, sizeof(detail),
                 ",\"rssi\":%d,\"auth\":\"%s\",\"channel\":%u}",
                 records[i].rssi,
                 authModeName(records[i].authmode),
                 records[i].primary);
        strlcat(json, detail, kWifiScanJsonSize);
        first = false;
    }
    strlcat(json, "]}", kWifiScanJsonSize);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t sendErr = httpd_resp_sendstr(req, json);
    heap_caps_free(json);
    heap_caps_free(records);
    return sendErr == ESP_OK ? ESP_OK : ESP_FAIL;
}

static void saveFormText(const char *body, const char *key, char *dst, size_t dstSize, bool *ok) {
    char value[160];
    if (!formValue(body, key, value, sizeof(value))) {
        return;
    }
    if (!appSaveTextSetting(key, value)) {
        *ok = false;
        return;
    }
    size_t len = strlen(value);
    if (len >= dstSize) {
        len = dstSize - 1;
    }
    memcpy(dst, value, len);
    dst[len] = '\0';
}

static bool formEnabledValue(const char *body, const char *key, bool *enabled) {
    char value[16];
    if (!formValue(body, key, value, sizeof(value))) {
        return false;
    }
    if (strcasecmp(value, "on") == 0) {
        *enabled = true;
        return true;
    }
    if (strcasecmp(value, "off") == 0) {
        *enabled = false;
        return true;
    }
    return false;
}

static bool parseFormTimezone(const char *src, int32_t *offsetSeconds) {
    if (!src || (src[0] != '+' && src[0] != '-') ||
        src[1] < '0' || src[1] > '9' ||
        src[2] < '0' || src[2] > '9' ||
        src[3] != ':' ||
        src[4] < '0' || src[4] > '9' ||
        src[5] < '0' || src[5] > '9' ||
        src[6] != '\0') {
        return false;
    }
    int hours = ((src[1] - '0') * 10) + (src[2] - '0');
    int minutes = ((src[4] - '0') * 10) + (src[5] - '0');
    if (hours > 23 || minutes > 59) {
        return false;
    }
    int sign = src[0] == '-' ? -1 : 1;
    *offsetSeconds = sign * ((hours * 60 * 60) + (minutes * 60));
    return true;
}

static bool requestRuntimeFdMounts(const char oldFd[PV_FDD_DRIVE_COUNT][128],
                                   char *response,
                                   size_t responseSize) {
    if (!pvFddConfigured()) {
        return true;
    }

    bool ok = true;
    for (uint8_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
        if (strcmp(oldFd[drive], sSettings.fd[drive]) == 0) {
            continue;
        }

        char mounted[128];
        pvFddGetMountedName(drive, mounted, sizeof(mounted));
        char line[192];
        if (mounted[0]) {
            snprintf(line, sizeof(line),
                     "FD%u saved; eject current disk in Mac before live mount\n",
                     static_cast<unsigned>(drive));
            appendText(response, responseSize, line);
            continue;
        }

        if (!sSettings.fd[drive][0]) {
            snprintf(line, sizeof(line), "FD%u saved empty\n",
                     static_cast<unsigned>(drive));
            appendText(response, responseSize, line);
            continue;
        }

        if (pvFddRequestImage(drive, sSettings.fd[drive])) {
            snprintf(line, sizeof(line), "FD%u live mount queued: %s\n",
                     static_cast<unsigned>(drive), sSettings.fd[drive]);
            appendText(response, responseSize, line);
        } else {
            snprintf(line, sizeof(line), "FD%u live mount request failed\n",
                     static_cast<unsigned>(drive));
            appendText(response, responseSize, line);
            ok = false;
        }
    }
    return ok;
}

static esp_err_t settingsPostHandler(httpd_req_t *req) {
    constexpr size_t kSettingsBodySize = 3072;
    char *body = allocPsramTextBuffer(kSettingsBodySize, "settings body");
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram");
        return ESP_FAIL;
    }
    if (readRequestBody(req, body, kSettingsBodySize) < 0) {
        heap_caps_free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad settings");
        return ESP_FAIL;
    }

    char oldFd[PV_FDD_DRIVE_COUNT][128];
    for (uint8_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
        snprintf(oldFd[drive], sizeof(oldFd[drive]), "%s", sSettings.fd[drive]);
    }

    bool ok = true;
    saveFormText(body, "ssid", sSettings.ssid, sizeof(sSettings.ssid), &ok);
    saveFormText(body, "password", sSettings.password, sizeof(sSettings.password), &ok);
    saveFormText(body, "server", sSettings.server, sizeof(sSettings.server), &ok);
    saveFormText(body, "rom", sSettings.rom, sizeof(sSettings.rom), &ok);
    saveFormText(body, "fd0", sSettings.fd[0], sizeof(sSettings.fd[0]), &ok);
    saveFormText(body, "fd1", sSettings.fd[1], sizeof(sSettings.fd[1]), &ok);
    for (int id = 0; id < SCSI_TARGET_COUNT; ++id) {
        char key[4];
        snprintf(key, sizeof(key), "hd%d", id);
        saveFormText(body, key, sSettings.hd[id], sizeof(sSettings.hd[id]), &ok);
    }

    char tz[32];
    if (formValue(body, "tz", tz, sizeof(tz))) {
        int32_t tzOffsetSeconds;
        if (parseFormTimezone(tz, &tzOffsetSeconds) &&
            appSaveTextSetting("tz", tz)) {
            sSettings.tzOffsetSeconds = tzOffsetSeconds;
        } else {
            ok = false;
        }
    }

    bool enabled;
    if (formEnabledValue(body, "log", &enabled)) {
        if (appSaveTextSetting("log", enabled ? "on" : "off")) {
            sSettings.consoleOut = enabled;
            appSetConsoleOutEnabled(enabled);
        } else {
            ok = false;
        }
    } else if (formValue(body, "log", nullptr, 0)) {
        ok = false;
    }
    if (formEnabledValue(body, "lcdflip", &enabled)) {
        if (appSaveTextSetting("lcdflip", enabled ? "on" : "off")) {
            sSettings.displayRotate180 = enabled;
        } else {
            ok = false;
        }
    } else if (formValue(body, "lcdflip", nullptr, 0)) {
        ok = false;
    }
    if (formEnabledValue(body, "turbo", &enabled)) {
        if (appSaveTextSetting("turbo", enabled ? "on" : "off")) {
            sSettings.turbo = enabled;
            appSetTurboEnabled(enabled);
        } else {
            ok = false;
        }
    } else if (formValue(body, "turbo", nullptr, 0)) {
        ok = false;
    }
    char audio[16];
    if (formValue(body, "audio", audio, sizeof(audio))) {
        AudioBackend audioBackend;
        if (appParseAudioBackend(audio, &audioBackend) &&
            appSaveTextSetting("audio", appAudioBackendName(audioBackend))) {
            sSettings.audioBackend = audioBackend;
            appSetAudioBackend(audioBackend);
        } else {
            ok = false;
        }
    }
    if (formEnabledValue(body, "boot_info", &enabled)) {
        if (appSaveTextSetting("boot_info", enabled ? "on" : "off")) {
            sSettings.bootInfo = enabled;
            appSetBootInfoEnabled(enabled);
        } else {
            ok = false;
        }
    } else if (formValue(body, "boot_info", nullptr, 0)) {
        ok = false;
    }

    char response[768] = {};
    snprintf(response, sizeof(response), "%s",
             ok ? "saved; reboot to apply boot-only settings\n" : "save failed\n");
    if (ok && !requestRuntimeFdMounts(oldFd, response, sizeof(response))) {
        ok = false;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, ok ? "200 OK" : "500 Internal Server Error");
    esp_err_t sendErr = httpd_resp_sendstr(req, response);
    heap_caps_free(body);
    if (sendErr != ESP_OK) {
        return ESP_FAIL;
    }
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t apiGetHandler(httpd_req_t *req) {
    if (uriPathEquals(req->uri, "/api/console/events")) {
        return consoleEventsHandler(req);
    }
    if (uriPathEquals(req->uri, "/api/settings")) {
        return settingsGetHandler(req);
    }
    if (uriPathEquals(req->uri, "/api/wifi/scan")) {
        return wifiScanHandler(req);
    }
    if (uriPathEquals(req->uri, "/api/files")) {
        return fileOptionsHandler(req);
    }
    if (uriPathEquals(req->uri, "/api/filer")) {
        return filerListHandler(req);
    }
    if (uriPathEquals(req->uri, "/api/filer/download")) {
        return filerDownloadHandler(req);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown api");
    return ESP_OK;
}

static esp_err_t apiPostHandler(httpd_req_t *req) {
    if (uriPathEquals(req->uri, "/api/console")) {
        return consoleApiHandler(req);
    }
    if (uriPathEquals(req->uri, "/api/settings")) {
        return settingsPostHandler(req);
    }
    if (uriPathEquals(req->uri, "/api/filer/mkdir")) {
        return filerMkdirHandler(req);
    }
    if (uriPathEquals(req->uri, "/api/filer/delete")) {
        return filerDeleteHandler(req);
    }
    if (uriPathEquals(req->uri, "/api/filer/rename")) {
        return filerRenameHandler(req);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown api");
    return ESP_OK;
}

static esp_err_t uploadHandler(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > kMaxUploadSize) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }
    char requested[160];
    char path[192];
    if (!queryPath(req, requested, sizeof(requested)) ||
        !mapUploadPath(requested, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }

    mkdir(kWebRoot, 0775);
    mkdir(kFdRoot, 0775);
    mkdir(kHdRoot, 0775);
    char tmpPath[208];
    snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
    FILE *fp = fopen(tmpPath, "wb");
    if (!fp) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }

    char *buf = sFileBuffer;
    size_t bufSize = kSendBufferSize;
    if (!buf) {
        fclose(fp);
        remove(tmpPath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int chunk = remaining < static_cast<int>(bufSize) ? remaining : static_cast<int>(bufSize);
        int received = httpd_req_recv(req, buf, chunk);
        if (received <= 0) {
            ok = false;
            break;
        }
        if (fwrite(buf, 1, received, fp) != static_cast<size_t>(received)) {
            ok = false;
            break;
        }
        remaining -= received;
    }
    if (fclose(fp) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(tmpPath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_FAIL;
    }
    remove(path);
    if (rename(tmpPath, path) != 0) {
        remove(tmpPath);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
        return ESP_FAIL;
    }

    printf("HTTP: uploaded %s (%d bytes)\n", path, req->content_len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static bool startHttpd(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.core_id = 0;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.max_open_sockets = 4;
    config.backlog_conn = 1;
    config.stack_size = 6144;
    config.send_wait_timeout = 1;
    config.recv_wait_timeout = 3;

    esp_err_t err = httpd_start(&sHttpd, &config);
    if (err != ESP_OK) {
        printf("HTTP: httpd_start failed: %s\n", esp_err_to_name(err));
        return false;
    }

    httpd_uri_t uploadPost = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = uploadHandler,
        .user_ctx = NULL
    };
    httpd_uri_t uploadPut = uploadPost;
    uploadPut.method = HTTP_PUT;
    httpd_uri_t apiGet = {
        .uri = "/api/*",
        .method = HTTP_GET,
        .handler = apiGetHandler,
        .user_ctx = NULL
    };
    httpd_uri_t apiPost = {
        .uri = "/api/*",
        .method = HTTP_POST,
        .handler = apiPostHandler,
        .user_ctx = NULL
    };
    httpd_uri_t get = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = getHandler,
        .user_ctx = NULL
    };
    httpd_uri_t *handlers[] = {
        &uploadPost,
        &uploadPut,
        &apiGet,
        &apiPost,
        &get,
    };
    for (httpd_uri_t *handler : handlers) {
        err = httpd_register_uri_handler(sHttpd, handler);
        if (err != ESP_OK) {
            printf("HTTP: register %s %d failed: %s\n",
                   handler->uri,
                   handler->method,
                   esp_err_to_name(err));
        }
    }
    printf("HTTP: server ready on core 0\n");
    logHttpHeap("after httpd start");
    return true;
}

bool macHttpNetworkStart(const AppSettings *cfg) {
    if (!sdcardMounted()) {
        printf("HTTP: SD card is not mounted; web files are unavailable\n");
    } else {
        appEnsureSdBootstrapFiles();
    }
    sSettings = *cfg;
    logHttpHeap("before file buffer");
    if (!sFileBuffer) {
        sFileBuffer = static_cast<char *>(heap_caps_aligned_alloc(kSdDmaAlignment,
                                                                  kSendBufferSize,
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!sFileBuffer) {
            printf("HTTP: PSRAM file buffer allocation failed, trying internal DMA\n");
            sFileBuffer = static_cast<char *>(heap_caps_aligned_alloc(kSdDmaAlignment,
                                                                      kSendBufferSize,
                                                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        }
        if (!sFileBuffer) {
            printf("HTTP: failed to allocate file buffer\n");
            return false;
        }
        printf("HTTP: file buffer %p size=%u align=%u psram=%s\n",
               sFileBuffer,
               static_cast<unsigned>(kSendBufferSize),
               static_cast<unsigned>(kSdDmaAlignment),
               esp_ptr_external_ram(sFileBuffer) ? "yes" : "no");
    }
    logHttpHeap("after file buffer");
    if (!connectWifi(cfg)) {
        return false;
    }
    return true;
}

bool macHttpStationConnected(void) {
    return sWifiConnected && !sSetupApMode;
}

bool macHttpReconnectWifi(const AppSettings *cfg) {
    if (sWifiStarted) {
        esp_wifi_stop();
        sWifiStarted = false;
    }
    sWifiConnected = false;
    sSetupApMode = false;
    sWifiRetryCount = 0;
    sIpAddress[0] = '\0';
    if (sWifiEventGroup) {
        xEventGroupClearBits(sWifiEventGroup, kWifiConnectedBit | kWifiFailedBit);
    }
    sSettings = *cfg;
    return connectWifi(cfg);
}

void macHttpDescribeWifi(char *response, size_t responseSize) {
    if (!response || responseSize == 0) {
        return;
    }
    appendText(response, responseSize, "WIFI: ");
    if (sWifiConnected) {
        appendText(response, responseSize, "station connected IP=");
        appendText(response, responseSize, sIpAddress[0] ? sIpAddress : "unknown");
        appendText(response, responseSize, "\n");
    } else if (sSetupApMode) {
        appendText(response, responseSize, "setup AP ");
        appendText(response, responseSize, kSetupApSsid);
        appendText(response, responseSize, " IP=");
        appendText(response, responseSize, kSetupApIpString);
        appendText(response, responseSize, "\n");
    } else if (sWifiStarted) {
        appendText(response, responseSize, "started, not connected\n");
    } else {
        appendText(response, responseSize, "stopped\n");
    }

    if (!ensureWifiInitialized()) {
        appendText(response, responseSize, "AP: Wi-Fi init failed\n");
        return;
    }
    if (sSetupApMode) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    } else if (!sWifiStarted) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        sWifiStarted = true;
    }

    wifi_scan_config_t scanConfig = {};
    esp_err_t err = esp_wifi_scan_start(&scanConfig, true);
    if (err != ESP_OK) {
        char line[80];
        snprintf(line, sizeof(line), "AP: scan failed: %s\n", esp_err_to_name(err));
        appendText(response, responseSize, line);
        return;
    }

    uint16_t apCount = 0;
    esp_wifi_scan_get_ap_num(&apCount);
    if (apCount > 12) {
        apCount = 12;
    }
    wifi_ap_record_t *records = allocWifiScanRecords(apCount, "console wifi scan");
    if (!records) {
        appendText(response, responseSize, "AP: scan records alloc failed\n");
        return;
    }
    uint16_t recordCount = apCount;
    err = esp_wifi_scan_get_ap_records(&recordCount, records);
    if (err != ESP_OK) {
        char line[80];
        snprintf(line, sizeof(line), "AP: records failed: %s\n", esp_err_to_name(err));
        appendText(response, responseSize, line);
        heap_caps_free(records);
        return;
    }
    appendText(response, responseSize, "AP list:\n");
    for (uint16_t i = 0; i < recordCount; ++i) {
        char ssid[33];
        memcpy(ssid, records[i].ssid, sizeof(ssid) - 1);
        ssid[sizeof(ssid) - 1] = '\0';
        if (ssid[0] == '\0') {
            continue;
        }
        char line[96];
        snprintf(line, sizeof(line), "  %s rssi=%d ch=%u auth=%s\n",
                 ssid,
                 records[i].rssi,
                 records[i].primary,
                 authModeName(records[i].authmode));
        appendText(response, responseSize, line);
    }
    heap_caps_free(records);
}

bool macHttpGetActiveUrl(char *url, size_t urlSize) {
    if (!url || urlSize == 0) {
        return false;
    }
    if (sSetupApMode) {
        snprintf(url, urlSize, "http://%s/", kSetupApIpString);
        return true;
    }
    if (sWifiConnected || sWifiStarted) {
        snprintf(url, urlSize, "http://%s.local/", kHostName);
        return true;
    }
    url[0] = '\0';
    return false;
}

bool macHttpServerStart(const AppSettings *cfg) {
    if (sHttpd) {
        return true;
    }
    if (!macHttpNetworkStart(cfg)) {
        return false;
    }
    startMdns();
    printf("HTTP: hostname set to %s; try http://%s.local/ or http://%s/\n",
           kHostName,
           kHostName,
           kHostName);
    return startHttpd();
}
