#include "net_time.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif_sntp.h"

extern "C" {
#include "tme/disp.h"
#include "tme/rtc.h"
}

static bool ntpSntpStarted = false;
static AppSettings ntpSettings;
static TaskHandle_t ntpTaskHandle;
static constexpr uint32_t kNtpSyncIntervalMs = 60UL * 60UL * 1000UL;

static void stopSntpForNtp() {
    if (ntpSntpStarted) {
        esp_netif_sntp_deinit();
        ntpSntpStarted = false;
    }
}

// Perform one boot-time NTP sync using the already-running Wi-Fi station.
// Steps:
// 1. start SNTP on the persistent Wi-Fi connection,
// 2. wait for network time,
// 3. push the result into both the host clock and guest RTC model,
// 4. stop only SNTP, leaving Wi-Fi alive for HTTP.
bool syncRtcFromNtpConfig(const AppSettings *ntpCfg) {
    printf("NTP: syncing via existing Wi-Fi on core %d, server \"%s\"\n",
           xPortGetCoreID(),
           ntpCfg->server);

    esp_sntp_config_t sntpConfig = ESP_NETIF_SNTP_DEFAULT_CONFIG(ntpCfg->server);
    sntpConfig.start = true;
    esp_err_t err = esp_netif_sntp_init(&sntpConfig);
    if (err != ESP_OK) {
        printf("NTP: SNTP init failed: %s\n", esp_err_to_name(err));
        return false;
    }
    ntpSntpStarted = true;

    bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK;
    if (ok) {
        // Keep both the ESP32 host clock and the Mac-side RTC view aligned.
        time_t unixNow = time(NULL);
        uint32_t macTime = (uint32_t)(unixNow + 2082844800UL + ntpCfg->tzOffsetSeconds);
        time_t localNow = unixNow + ntpCfg->tzOffsetSeconds;
        struct tm localTm;
        gmtime_r(&localNow, &localTm);
        rtcSetTimezoneOffset(ntpCfg->tzOffsetSeconds);
        rtcSetMacTime(macTime);
        printf("NTP: synced Unix=%lld Mac=0x%08lx tz=%ld\n",
               (long long)unixNow,
               (unsigned long)macTime,
               (long)ntpCfg->tzOffsetSeconds);
        dispConsolePrintf("NTP time %04d-%02d-%02d %02d:%02d:%02d\n",
                          localTm.tm_year + 1900,
                          localTm.tm_mon + 1,
                          localTm.tm_mday,
                          localTm.tm_hour,
                          localTm.tm_min,
                          localTm.tm_sec);
    } else {
        printf("NTP: sync timed out\n");
    }

    stopSntpForNtp();
    return ok;
}

static void periodicNtpTask(void *param) {
    (void)param;
    printf("NTP: periodic sync task running on core %d\n", xPortGetCoreID());
    while (true) {
        syncRtcFromNtpConfig(&ntpSettings);
        vTaskDelay(pdMS_TO_TICKS(kNtpSyncIntervalMs));
    }
}

bool startPeriodicNtpSync(const AppSettings *ntpCfg) {
    if (!ntpCfg) {
        return false;
    }
    memcpy(&ntpSettings, ntpCfg, sizeof(ntpSettings));
    if (ntpTaskHandle) {
        return true;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(periodicNtpTask,
                                            "ntp_sync",
                                            4096,
                                            NULL,
                                            1,
                                            &ntpTaskHandle,
                                            0);
    if (ok != pdPASS) {
        ntpTaskHandle = NULL;
        printf("NTP: failed to start periodic sync task\n");
        return false;
    }
    printf("NTP: periodic sync every 60 minutes on core 0\n");
    return true;
}
