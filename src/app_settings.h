#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "tme/ncr.h"

typedef enum AudioBackend {
    AUDIO_BACKEND_PWM = 0,
    AUDIO_BACKEND_PDM = 1,
    AUDIO_BACKEND_RMT = 2,
} AudioBackend;

typedef struct AppSettings {
    // Boot-time network settings.
    char server[64];
    // Wi-Fi credentials used for one-shot NTP sync.
    char ssid[33];
    char password[65];
    // Storage selections resolved during startup.
    char rom[128];
    // SCSI target images configured by hd0 through hd6.
    char hd[SCSI_TARGET_COUNT][128];
    // Internal/external raw 400K/800K floppy images.
    char fd[2][128];
    // Host-to-Mac timezone offset in seconds.
    int32_t tzOffsetSeconds;
    // Whether serial log output stays visible on the console.
    bool consoleOut;
    // LCD orientation flag parsed from setting.txt.
    bool displayRotate180;
    // Whether silent frames may skip realtime audio pacing.
    bool turbo;
    // Audio output backend selected at boot.
    AudioBackend audioBackend;
    // Whether boot diagnostics are shown on the LCD console.
    bool bootInfo;
} AppSettings;

#ifdef __cplusplus
extern "C" {
#endif

// Fill every setting with a safe default before parsing /sd/setting.txt.
void appSettingsInitDefaults(AppSettings *cfg);
// Create SD bootstrap directories and default config files when missing.
bool appEnsureSdBootstrapFiles(void);
// Parse the boot settings file and update the startup configuration.
bool loadSettingsFromSd(AppSettings *cfg);
// Read one saved Wi-Fi credential from /sd/wifilist.txt.
bool appLoadWifiListEntry(int index, char *ssid, size_t ssidSize, char *password, size_t passwordSize);
// Remember a successfully connected Wi-Fi credential, newest first, max 10.
bool appRememberWifiNetwork(const char *ssid, const char *password);
// Resolve a config filename relative to /sd/ unless it is already absolute.
const char *resolveSdConfigPath(const char *configuredName,
                                const char *defaultName,
                                char *buf,
                                size_t bufSize);
// Persist one storage image selection while preserving other settings.
bool appSaveStorageSelection(const char *key, const char *imageName);
// Persist one user-editable boot setting while preserving other settings.
bool appSaveTextSetting(const char *key, const char *value);
// Persist one on/off setting while preserving other settings.
bool appSaveBoolSetting(const char *key, bool enabled);
// Check whether host log output is currently enabled.
bool appConsoleOutEnabled(void);
// Toggle host log output from the serial console.
void appSetConsoleOutEnabled(bool enabled);
// Check whether silent-frame turbo is enabled.
bool appTurboEnabled(void);
// Toggle silent-frame turbo at runtime.
void appSetTurboEnabled(bool enabled);
// Check which audio backend should be used.
AudioBackend appAudioBackend(void);
// Select the audio backend for the next emulator start in this process.
void appSetAudioBackend(AudioBackend backend);
// Parse/format audio backend setting values.
bool appParseAudioBackend(const char *value, AudioBackend *backend);
const char *appAudioBackendName(AudioBackend backend);
// Check whether boot diagnostics should be shown on the LCD console.
bool appBootInfoEnabled(void);
// Toggle boot diagnostics for the next boot and current menu display.
void appSetBootInfoEnabled(bool enabled);
// Check whether the emulator is currently skipping realtime VBL sync.
bool appRealtimeSyncSkipped(void);
// Publish the current auto no-sync state for status displays.
void appSetRealtimeSyncSkipped(bool skipped);

#ifdef __cplusplus
}
#endif
