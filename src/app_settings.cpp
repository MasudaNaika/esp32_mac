#include "app_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "tme/tmeconfig.h"

#ifndef ESP32_MAC_TZ_OFFSET_SECONDS
#define ESP32_MAC_TZ_OFFSET_SECONDS (9 * 60 * 60)
#endif

#define SETTINGS_PATH "/sd/setting.txt"
#define WIFILIST_PATH "/sd/wifilist.txt"
#define WIFILIST_MAX_ENTRIES 10

static bool gConsoleOutEnabled = true;
static bool gTurboEnabled = true;
static AudioBackend gAudioBackend = AUDIO_BACKEND_PWM;
static bool gBootInfoEnabled = true;
static bool gRealtimeSyncSkipped = false;

// Trim leading and trailing ASCII whitespace in-place.
// The settings parser uses this before comparing keys and values.
static char *trimLine(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

// Treat '#' as a comment marker at the start of a trimmed line or after
// whitespace. Keep embedded characters such as password=abc#123 intact.
static void stripSettingComment(char *s) {
    for (char *p = s; *p; ++p) {
        if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
            *p = '\0';
            return;
        }
    }
}

// Copy one text setting into a fixed-size destination buffer safely.
static void copyConfigValue(char *dst, size_t dstSize, const char *src) {
    if (dstSize == 0) {
        return;
    }
    size_t len = strlen(src);
    if (len >= dstSize) {
        len = dstSize - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool fileExists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool ensureDir(const char *path) {
    if (mkdir(path, 0775) == 0) {
        return true;
    }
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void formatTimezone(int32_t offsetSeconds, char *buf, size_t bufSize) {
    char sign = offsetSeconds < 0 ? '-' : '+';
    int32_t absSeconds = offsetSeconds < 0 ? -offsetSeconds : offsetSeconds;
    snprintf(buf, bufSize, "%c%02ld:%02ld",
             sign,
             (long)(absSeconds / 3600),
             (long)((absSeconds % 3600) / 60));
}

static bool writeDefaultSettingsFile(void) {
    FILE *fp = fopen(SETTINGS_PATH, "w");
    if (!fp) {
        return false;
    }

    char tz[16];
    formatTimezone(ESP32_MAC_TZ_OFFSET_SECONDS, tz, sizeof(tz));
    fprintf(fp, "server=pool.ntp.org\n");
    fprintf(fp, "ssid=\n");
    fprintf(fp, "password=\n");
    fprintf(fp, "tz=%s\n", tz);
    fprintf(fp, "rom=macplus.rom\n");
    for (int id = 0; id < 2; ++id) {
        fprintf(fp, "fd%d=\n", id);
    }
    for (int id = 0; id < SCSI_TARGET_COUNT; ++id) {
        fprintf(fp, "hd%d=%s\n", id, id == 6 ? "hd/hd.img" : "");
    }
    fprintf(fp, "log=on\n");
    fprintf(fp, "lcdflip=on\n");
    fprintf(fp, "turbo=on\n");
    fprintf(fp, "audio=pwm\n");
    fprintf(fp, "boot_info=on\n");

    return fclose(fp) == 0;
}

bool appEnsureSdBootstrapFiles(void) {
    bool ok = true;
    ok = ensureDir("/sd/web") && ok;
    ok = ensureDir("/sd/fd") && ok;
    ok = ensureDir("/sd/hd") && ok;

    if (!fileExists(SETTINGS_PATH)) {
        if (writeDefaultSettingsFile()) {
            printf("SETTINGS: created %s\n", SETTINGS_PATH);
        } else {
            printf("SETTINGS: failed to create %s\n", SETTINGS_PATH);
            ok = false;
        }
    }
    if (!fileExists(WIFILIST_PATH)) {
        FILE *fp = fopen(WIFILIST_PATH, "w");
        if (fp) {
            fclose(fp);
            printf("SETTINGS: created %s\n", WIFILIST_PATH);
        } else {
            printf("SETTINGS: failed to create %s\n", WIFILIST_PATH);
            ok = false;
        }
    }
    return ok;
}

// Parse timezone values in +09:00 / -05:30 form.
static bool parseTimezoneOffset(const char *src, int32_t *offsetSeconds) {
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

// Parse the on/off value shared by multiple settings keys.
static bool parseEnabledValue(const char *src, bool *enabled) {
    if (strcasecmp(src, "on") == 0) {
        *enabled = true;
        return true;
    }
    if (strcasecmp(src, "off") == 0) {
        *enabled = false;
        return true;
    }
    return false;
}

bool appParseAudioBackend(const char *value, AudioBackend *backend) {
    if (!value || !backend) {
        return false;
    }
    if (strcasecmp(value, "pwm") == 0) {
        *backend = AUDIO_BACKEND_PWM;
        return true;
    }
    if (strcasecmp(value, "pdm") == 0 || strcasecmp(value, "i2s_pdm") == 0) {
        *backend = AUDIO_BACKEND_PDM;
        return true;
    }
    if (strcasecmp(value, "rmt") == 0) {
        *backend = AUDIO_BACKEND_RMT;
        return true;
    }
    return false;
}

const char *appAudioBackendName(AudioBackend backend) {
    switch (backend) {
    case AUDIO_BACKEND_PDM:
        return "pdm";
    case AUDIO_BACKEND_RMT:
        return "rmt";
    case AUDIO_BACKEND_PWM:
    default:
        return "pwm";
    }
}

// Fill the settings struct with safe defaults before parsing user overrides.
void appSettingsInitDefaults(AppSettings *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    copyConfigValue(cfg->server, sizeof(cfg->server), "pool.ntp.org");
    copyConfigValue(cfg->rom, sizeof(cfg->rom), "macplus.rom");
    copyConfigValue(cfg->hd[6], sizeof(cfg->hd[6]), "hd/hd.img");
    cfg->tzOffsetSeconds = ESP32_MAC_TZ_OFFSET_SECONDS;
    cfg->consoleOut = true;
    cfg->displayRotate180 = true;
    cfg->turbo = true;
    cfg->audioBackend = AUDIO_BACKEND_PWM;
    cfg->bootInfo = true;
}

// Load boot settings from `/sd/setting.txt`.
// Steps:
// 1. start from defaults,
// 2. parse `key=value` pairs when present,
// 3. keep the old positional format for backward compatibility.
bool loadSettingsFromSd(AppSettings *cfg) {
    appSettingsInitDefaults(cfg);

    FILE *fp = fopen(SETTINGS_PATH, "r");
    if (!fp) {
        printf("SETTINGS: %s not found, skipping time sync\n", SETTINGS_PATH);
        return false;
    }

    char line[160];
    int positional = 0;
    while (fgets(line, sizeof(line), fp)) {
        stripSettingComment(line);
        char *s = trimLine(line);
        if (s[0] == '\0') {
            continue;
        }

        char *eq = strchr(s, '=');
        if (eq) {
            // Prefer explicit key=value entries and keep positional parsing only
            // for older setting files.
            *eq = '\0';
            char *key = trimLine(s);
            char *value = trimLine(eq + 1);
            if (strcasecmp(key, "server") == 0) {
                copyConfigValue(cfg->server, sizeof(cfg->server), value);
            } else if (strcasecmp(key, "ssid") == 0) {
                copyConfigValue(cfg->ssid, sizeof(cfg->ssid), value);
            } else if (strcasecmp(key, "password") == 0) {
                copyConfigValue(cfg->password, sizeof(cfg->password), value);
            } else if (strcasecmp(key, "rom") == 0) {
                copyConfigValue(cfg->rom, sizeof(cfg->rom), value);
            } else if (strncasecmp(key, "hd", 2) == 0 && key[2] >= '0' &&
                       key[2] < ('0' + SCSI_TARGET_COUNT) && key[3] == '\0') {
                int id = key[2] - '0';
                copyConfigValue(cfg->hd[id], sizeof(cfg->hd[id]), value);
            } else if (strcasecmp(key, "fd0") == 0) {
                copyConfigValue(cfg->fd[0], sizeof(cfg->fd[0]), value);
            } else if (strcasecmp(key, "fd1") == 0) {
                copyConfigValue(cfg->fd[1], sizeof(cfg->fd[1]), value);
            } else if (strcasecmp(key, "tz") == 0) {
                int32_t tzOffsetSeconds;
                if (parseTimezoneOffset(value, &tzOffsetSeconds)) {
                    cfg->tzOffsetSeconds = tzOffsetSeconds;
                } else {
                    printf("NTP: invalid timezone \"%s\", keeping default tz=%ld\n",
                           value,
                           (long)cfg->tzOffsetSeconds);
                }
            } else if (strcasecmp(key, "log") == 0) {
                bool consoleOut;
                if (parseEnabledValue(value, &consoleOut)) {
                    cfg->consoleOut = consoleOut;
                } else {
                    printf("SETTINGS: invalid log \"%s\", keeping enabled\n", value);
                }
            } else if (strcasecmp(key, "lcdflip") == 0) {
                bool displayRotate180;
                if (parseEnabledValue(value, &displayRotate180)) {
                    cfg->displayRotate180 = displayRotate180;
                } else {
                    printf("SETTINGS: invalid lcdflip \"%s\", keeping default\n", value);
                }
            } else if (strcasecmp(key, "turbo") == 0) {
                bool turbo;
                if (parseEnabledValue(value, &turbo)) {
                    cfg->turbo = turbo;
                } else {
                    printf("SETTINGS: invalid turbo \"%s\", keeping default\n", value);
                }
            } else if (strcasecmp(key, "audio") == 0) {
                AudioBackend audioBackend;
                if (appParseAudioBackend(value, &audioBackend)) {
                    cfg->audioBackend = audioBackend;
                } else {
                    printf("SETTINGS: invalid audio \"%s\", keeping default\n", value);
                }
            } else if (strcasecmp(key, "boot_info") == 0) {
                bool bootInfo;
                if (parseEnabledValue(value, &bootInfo)) {
                    cfg->bootInfo = bootInfo;
                } else {
                    printf("SETTINGS: invalid boot_info \"%s\", keeping default\n", value);
                }
            }
            continue;
        }

        // Backward-compatible layout: server, ssid, password by line order.
        if (positional == 0) {
            copyConfigValue(cfg->server, sizeof(cfg->server), s);
        } else if (positional == 1) {
            copyConfigValue(cfg->ssid, sizeof(cfg->ssid), s);
        } else if (positional == 2) {
            copyConfigValue(cfg->password, sizeof(cfg->password), s);
        }
        positional++;
    }
    fclose(fp);

    printf("SETTINGS: log=%s\n", cfg->consoleOut ? "on" : "off");
    printf("SETTINGS: lcdflip=%s\n", cfg->displayRotate180 ? "on" : "off");
    printf("SETTINGS: turbo=%s\n", cfg->turbo ? "on" : "off");
    printf("SETTINGS: audio=%s\n", appAudioBackendName(cfg->audioBackend));
    printf("SETTINGS: boot_info=%s\n", cfg->bootInfo ? "on" : "off");
    printf("SETTINGS: rom=%s\n", cfg->rom);
    for (int id = 0; id < 2; ++id) {
        printf("SETTINGS: fd%d=%s\n", id, cfg->fd[id][0] ? cfg->fd[id] : "off");
    }
    for (int id = 0; id < SCSI_TARGET_COUNT; ++id) {
        printf("SETTINGS: hd%d=%s\n", id, cfg->hd[id][0] ? cfg->hd[id] : "off");
    }
    printf("NTP: config server=%s ssid=%s tz=%ld\n",
           cfg->server,
           cfg->ssid,
           (long)cfg->tzOffsetSeconds);
    return true;
}

// Resolve a configured filename into an SD-card path.
// Relative names are rooted under `/sd/`, while absolute paths pass through.
const char *resolveSdConfigPath(const char *configuredName,
                                const char *defaultName,
                                char *buf,
                                size_t bufSize) {
    const char *name = (configuredName && configuredName[0] != '\0') ? configuredName : defaultName;
    if (!name || name[0] == '\0') {
        return NULL;
    }
    if (name[0] == '/') {
        return name;
    }
    snprintf(buf, bufSize, "/sd/%s", name);
    return buf;
}

static bool saveSettingValue(const char *key, const char *value) {
    if (!key || !value || strchr(value, '\n') || strchr(value, '\r')) {
        return false;
    }

    constexpr const char *kTempPath = "/sd/setting.tmp";
    constexpr const char *kBackupPath = "/sd/setting.bak";
    FILE *input = fopen(SETTINGS_PATH, "r");
    FILE *output = fopen(kTempPath, "w");
    if (!output) {
        if (input) fclose(input);
        return false;
    }

    bool replaced = false;
    char line[192];
    while (input && fgets(line, sizeof(line), input)) {
        char parsed[192];
        snprintf(parsed, sizeof(parsed), "%s", line);
        char *s = trimLine(parsed);
        char *eq = strchr(s, '=');
        if (eq) {
            *eq = '\0';
            const char *parsedKey = trimLine(s);
            bool matches = strcasecmp(parsedKey, key) == 0;
            if (matches) {
                fprintf(output, "%s=%s\n", key, value);
                replaced = true;
                continue;
            }
        }
        fputs(line, output);
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] != '\n') {
            fputc('\n', output);
        }
    }
    if (input) fclose(input);
    if (!replaced) {
        fprintf(output, "%s=%s\n", key, value);
    }
    if (fclose(output) != 0) {
        remove(kTempPath);
        return false;
    }

    remove(kBackupPath);
    if (rename(SETTINGS_PATH, kBackupPath) != 0 && input) {
        remove(kTempPath);
        return false;
    }
    if (rename(kTempPath, SETTINGS_PATH) != 0) {
        rename(kBackupPath, SETTINGS_PATH);
        return false;
    }
    remove(kBackupPath);
    printf("SETTINGS: selected %s=%s\n", key, value);
    return true;
}

// Rewrite only hdN/fdN through a temporary file so menu selections take
// effect on the next clean emulator restart without losing unrelated settings.
bool appSaveStorageSelection(const char *key, const char *imageName) {
    bool validKey = key && ((key[0] == 'f' && key[1] == 'd' &&
                    (key[2] == '0' || key[2] == '1') && key[3] == '\0') ||
                    (key[0] == 'h' && key[1] == 'd' && key[2] >= '0' &&
                     key[2] < ('0' + SCSI_TARGET_COUNT) && key[3] == '\0'));
    if (!validKey) {
        return false;
    }
    return saveSettingValue(key, imageName);
}

bool appSaveTextSetting(const char *key, const char *value) {
    if (!key || !value) {
        return false;
    }
    bool validKey = strcasecmp(key, "server") == 0 ||
                    strcasecmp(key, "ssid") == 0 ||
                    strcasecmp(key, "password") == 0 ||
                    strcasecmp(key, "tz") == 0 ||
                    strcasecmp(key, "rom") == 0 ||
                    strcasecmp(key, "log") == 0 ||
                    strcasecmp(key, "lcdflip") == 0 ||
                    strcasecmp(key, "turbo") == 0 ||
                    strcasecmp(key, "audio") == 0 ||
                    strcasecmp(key, "boot_info") == 0 ||
                    strcasecmp(key, "fd0") == 0 ||
                    strcasecmp(key, "fd1") == 0;
    if (!validKey && strncasecmp(key, "hd", 2) == 0 && key[2] >= '0' &&
        key[2] < ('0' + SCSI_TARGET_COUNT) && key[3] == '\0') {
        validKey = true;
    }
    if (!validKey) {
        return false;
    }
    return saveSettingValue(key, value);
}

typedef struct WifiListEntry {
    char ssid[33];
    char password[65];
} WifiListEntry;

static WifiListEntry *sWifiReadEntries;
static WifiListEntry *sWifiOldEntries;
static WifiListEntry *sWifiNewEntries;

static bool ensureWifiListBuffer(WifiListEntry **buffer) {
    if (*buffer) {
        memset(*buffer, 0, sizeof(WifiListEntry) * WIFILIST_MAX_ENTRIES);
        return true;
    }
    *buffer = static_cast<WifiListEntry *>(tme_psram_aligned_calloc(WIFILIST_MAX_ENTRIES,
                                                                    sizeof(WifiListEntry),
                                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!*buffer) {
        printf("SETTINGS: failed to allocate Wi-Fi list PSRAM buffer\n");
        return false;
    }
    return true;
}

static bool readWifiList(WifiListEntry *entries, int *count) {
    *count = 0;
    FILE *fp = fopen(WIFILIST_PATH, "r");
    if (!fp) {
        return false;
    }
    char line[128];
    while (*count < WIFILIST_MAX_ENTRIES && fgets(line, sizeof(line), fp)) {
        char *s = trimLine(line);
        if (s[0] == '\0' || s[0] == '#') {
            continue;
        }
        char *tab = strchr(s, '\t');
        if (!tab) {
            tab = strchr(s, ',');
        }
        if (!tab) {
            continue;
        }
        *tab = '\0';
        char *ssid = trimLine(s);
        char *password = trimLine(tab + 1);
        if (ssid[0] == '\0') {
            continue;
        }
        copyConfigValue(entries[*count].ssid, sizeof(entries[*count].ssid), ssid);
        copyConfigValue(entries[*count].password, sizeof(entries[*count].password), password);
        (*count)++;
    }
    fclose(fp);
    return true;
}

static bool writeWifiList(const WifiListEntry *entries, int count) {
    constexpr const char *kTempPath = "/sd/wifilist.tmp";
    constexpr const char *kBackupPath = "/sd/wifilist.bak";
    FILE *fp = fopen(kTempPath, "w");
    if (!fp) {
        return false;
    }
    for (int i = 0; i < count && i < WIFILIST_MAX_ENTRIES; ++i) {
        if (entries[i].ssid[0] == '\0') {
            continue;
        }
        fprintf(fp, "%s\t%s\n", entries[i].ssid, entries[i].password);
    }
    if (fclose(fp) != 0) {
        remove(kTempPath);
        return false;
    }
    remove(kBackupPath);
    rename(WIFILIST_PATH, kBackupPath);
    if (rename(kTempPath, WIFILIST_PATH) != 0) {
        rename(kBackupPath, WIFILIST_PATH);
        return false;
    }
    remove(kBackupPath);
    return true;
}

bool appLoadWifiListEntry(int index, char *ssid, size_t ssidSize, char *password, size_t passwordSize) {
    if (index < 0 || !ssid || !password || ssidSize == 0 || passwordSize == 0) {
        return false;
    }
    int count = 0;
    if (!ensureWifiListBuffer(&sWifiReadEntries)) {
        return false;
    }
    if (!readWifiList(sWifiReadEntries, &count) || index >= count) {
        return false;
    }
    copyConfigValue(ssid, ssidSize, sWifiReadEntries[index].ssid);
    copyConfigValue(password, passwordSize, sWifiReadEntries[index].password);
    return true;
}

bool appRememberWifiNetwork(const char *ssid, const char *password) {
    if (!ssid || ssid[0] == '\0' || strchr(ssid, '\n') || strchr(ssid, '\r') ||
        strchr(ssid, '\t') || (password && (strchr(password, '\n') || strchr(password, '\r') ||
        strchr(password, '\t')))) {
        return false;
    }

    if (!ensureWifiListBuffer(&sWifiOldEntries) || !ensureWifiListBuffer(&sWifiNewEntries)) {
        return false;
    }
    int oldCount = 0;
    readWifiList(sWifiOldEntries, &oldCount);

    copyConfigValue(sWifiNewEntries[0].ssid, sizeof(sWifiNewEntries[0].ssid), ssid);
    copyConfigValue(sWifiNewEntries[0].password, sizeof(sWifiNewEntries[0].password), password ? password : "");
    int newCount = 1;
    for (int i = 0; i < oldCount && newCount < WIFILIST_MAX_ENTRIES; ++i) {
        if (strcmp(sWifiOldEntries[i].ssid, ssid) == 0) {
            continue;
        }
        sWifiNewEntries[newCount++] = sWifiOldEntries[i];
    }

    if (!writeWifiList(sWifiNewEntries, newCount)) {
        return false;
    }
    printf("SETTINGS: remembered Wi-Fi SSID %s\n", ssid);
    return true;
}

bool appSaveBoolSetting(const char *key, bool enabled) {
    if (!key || (strcasecmp(key, "turbo") != 0 &&
                 strcasecmp(key, "boot_info") != 0)) {
        return false;
    }
    return saveSettingValue(key, enabled ? "on" : "off");
}

// Report whether serial log output is currently enabled.
bool appConsoleOutEnabled(void) {
    return gConsoleOutEnabled;
}

// Enable or disable serial log output at runtime.
void appSetConsoleOutEnabled(bool enabled) {
    gConsoleOutEnabled = enabled;
}

// Report whether silent-frame turbo is enabled.
bool appTurboEnabled(void) {
    return gTurboEnabled;
}

void appSetTurboEnabled(bool enabled) {
    gTurboEnabled = enabled;
}

AudioBackend appAudioBackend(void) {
    return gAudioBackend;
}

void appSetAudioBackend(AudioBackend backend) {
    switch (backend) {
    case AUDIO_BACKEND_PDM:
    case AUDIO_BACKEND_RMT:
        gAudioBackend = backend;
        break;
    case AUDIO_BACKEND_PWM:
    default:
        gAudioBackend = AUDIO_BACKEND_PWM;
        break;
    }
}

bool appBootInfoEnabled(void) {
    return gBootInfoEnabled;
}

void appSetBootInfoEnabled(bool enabled) {
    gBootInfoEnabled = enabled;
}

// Report whether realtime VBL sync is currently being skipped.
bool appRealtimeSyncSkipped(void) {
    return gRealtimeSyncSkipped;
}

// Publish the current auto no-sync state for status displays.
void appSetRealtimeSyncSkipped(bool skipped) {
    gRealtimeSyncSkipped = skipped;
}
