#include "console_shell.h"

#ifndef ESP32_MAC_OPCODE_PROFILER
#define ESP32_MAC_OPCODE_PROFILER 0
#endif

#if ESP32_MAC_OPCODE_PROFILER
#include <cstdlib>
#endif
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "app_settings.h"
#include "http_server.h"
#include "net_time.h"
#include "storage_usb.h"
#include "tme/emu_monitor.h"
#include "tme/tmeconfig.h"

extern "C" {
#include "tme/disp.h"
#if ESP32_MAC_OPCODE_PROFILER
#include "tme/musashi/m68k.h"
#endif
}

static ConsoleShellRomSourceGetter gRomSourceGetter = nullptr;
static bool gRestartScheduled = false;
static portMUX_TYPE gRestartMux = portMUX_INITIALIZER_UNLOCKED;

// Let HTTP callers receive the command response before the network is stopped.
static void delayedRestartTask(void *param) {
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static bool scheduleRestart() {
    taskENTER_CRITICAL(&gRestartMux);
    if (gRestartScheduled) {
        taskEXIT_CRITICAL(&gRestartMux);
        return true;
    }
    gRestartScheduled = true;
    taskEXIT_CRITICAL(&gRestartMux);

    TaskHandle_t restartTask = nullptr;
    BaseType_t ok = xTaskCreatePinnedToCore(delayedRestartTask, "delayed_reboot", 2048,
                                            nullptr, 5, &restartTask, 0);
    if (ok != pdPASS) {
        taskENTER_CRITICAL(&gRestartMux);
        gRestartScheduled = false;
        taskEXIT_CRITICAL(&gRestartMux);
    }
    return ok == pdPASS;
}

// Trim leading and trailing ASCII whitespace from one command line.
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

// Small polling sleep used by the serial command task.
static void delayMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// Show a simple prompt so the serial shell feels alive even before typing.
static void printPrompt() {
    printf("console> ");
    fflush(stdout);
}

// Resolve the current ROM source string through the injected callback.
static const char *currentRomSource() {
    return gRomSourceGetter ? gRomSourceGetter() : "unknown";
}

// Print the built-in shell command list to both serial and onscreen console.
static void appendResponse(char *response, size_t responseSize, const char *text) {
    if (!response || responseSize == 0 || !text) {
        return;
    }
    size_t used = strlen(response);
    if (used >= responseSize - 1) {
        return;
    }
    snprintf(response + used, responseSize - used, "%s", text);
}

static void printConsoleCommandHelp(char *response = nullptr, size_t responseSize = 0) {
#if ESP32_MAC_OPCODE_PROFILER
    printf("Commands: help, cls, console, emu, storage, log on/off, perf on/off, cpu on/off, opprof on/off/reset/dump [n], status, show wifi, set wifi SSID PASSWORD, connect wifi, reboot\n");
    appendResponse(response, responseSize, "Commands: help, cls, console, emu, storage, log on/off, perf on/off, cpu on/off, opprof on/off/reset/dump [n], status, show wifi, set wifi SSID PASSWORD, connect wifi, reboot\n");
#else
    printf("Commands: help, cls, console, emu, storage, log on/off, perf on/off, cpu on/off, turbo on/off, status, show wifi, set wifi SSID PASSWORD, connect wifi, reboot\n");
    appendResponse(response, responseSize, "Commands: help, cls, console, emu, storage, log on/off, perf on/off, cpu on/off, turbo on/off, status, show wifi, set wifi SSID PASSWORD, connect wifi, reboot\n");
#endif
    dispConsolePrintf("Commands:\n");
    dispConsolePrintf("help cls console\n");
    dispConsolePrintf("emu storage log on/off\n");
    dispConsolePrintf("perf on/off\n");
    dispConsolePrintf("cpu on/off\n");
    dispConsolePrintf("turbo on/off\n");
    dispConsolePrintf("show/set/connect wifi\n");
    dispConsolePrintf("reboot\n");
#if ESP32_MAC_OPCODE_PROFILER
    dispConsolePrintf("opprof on/off/dump\n");
#endif
    dispConsolePrintf("status\n");
}

static void printWifiStatus(char *response = nullptr, size_t responseSize = 0) {
    constexpr size_t kWifiStatusTextSize = 2048;
    char *text = static_cast<char *>(tme_psram_aligned_calloc(1,
                                                              kWifiStatusTextSize,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!text) {
        printf("WiFi status buffer allocation failed\n");
        dispConsolePrintf("WiFi status alloc failed\n");
        appendResponse(response, responseSize, "WiFi status buffer allocation failed\n");
        return;
    }
    macHttpDescribeWifi(text, kWifiStatusTextSize);
    printf("%s", text);
    dispConsolePrintf("WiFi status printed\n");
    appendResponse(response, responseSize, text);
    heap_caps_free(text);
}

static bool parseSetWifiArgs(char *args, char **ssid, char **password) {
    args = trimLine(args);
    if (args[0] == '\0') {
        return false;
    }
    char *space = strpbrk(args, " \t");
    if (!space) {
        return false;
    }
    *space++ = '\0';
    space = trimLine(space);
    if (space[0] == '\0') {
        return false;
    }
    *ssid = args;
    *password = space;
    return true;
}

static bool reloadSettings(AppSettings *cfg) {
    appSettingsInitDefaults(cfg);
    appEnsureSdBootstrapFiles();
    return loadSettingsFromSd(cfg);
}

// Print a concise runtime status summary.
// This includes ROM source, log/perf flags, sync mode, and memory availability.
static void printConsoleStatus(char *response = nullptr, size_t responseSize = 0) {
    int freeMem;
    int largestMem;

    printf("STATUS: rom=%s log=%s perf=%s cpu=%s sync=%s turbo=%s\n",
           currentRomSource(),
           appConsoleOutEnabled() ? "on" : "off",
           emuMonitorDetailedEnabled() ? "on" : "off",
           emuMonitorCoreUsageEnabled() ? "on" : "off",
           appRealtimeSyncSkipped() ? "skip" : "audio",
           appTurboEnabled() ? "on" : "off");
    if (response && responseSize > 0) {
        snprintf(response, responseSize,
                 "STATUS: rom=%s log=%s perf=%s cpu=%s sync=%s turbo=%s\n",
                 currentRomSource(),
                 appConsoleOutEnabled() ? "on" : "off",
                 emuMonitorDetailedEnabled() ? "on" : "off",
                 emuMonitorCoreUsageEnabled() ? "on" : "off",
                 appRealtimeSyncSkipped() ? "skip" : "audio",
                 appTurboEnabled() ? "on" : "off");
    }
    dispConsolePrintf("STATUS rom=%s log=%s perf=%s cpu=%s sync=%s turbo=%s\n",
                      currentRomSource(),
                      appConsoleOutEnabled() ? "on" : "off",
                      emuMonitorDetailedEnabled() ? "on" : "off",
                      emuMonitorCoreUsageEnabled() ? "on" : "off",
                      appRealtimeSyncSkipped() ? "skip" : "audio",
                      appTurboEnabled() ? "on" : "off");
#if ESP32_MAC_OPCODE_PROFILER
    printf("STATUS: opprof=%s\n", m68k_opcode_profiler_enabled() ? "on" : "off");
    dispConsolePrintf("OPPROF %s\n", m68k_opcode_profiler_enabled() ? "on" : "off");
#endif

    freeMem = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    largestMem = (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    printf("STATUS: DRAM free=%d largest=%d\n", freeMem, largestMem);
    dispConsolePrintf("DRAM %d / %d\n", freeMem, largestMem);
    if (response && responseSize > 0) {
        char line[80];
        snprintf(line, sizeof(line), "STATUS: DRAM free=%d largest=%d\n", freeMem, largestMem);
        appendResponse(response, responseSize, line);
    }

    freeMem = (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    largestMem = (int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    printf("STATUS: PSRAM free=%d largest=%d\n", freeMem, largestMem);
    dispConsolePrintf("PSRAM %d / %d\n", freeMem, largestMem);
    if (response && responseSize > 0) {
        char line[80];
        snprintf(line, sizeof(line), "STATUS: PSRAM free=%d largest=%d\n", freeMem, largestMem);
        appendResponse(response, responseSize, line);
    }
}

// Dispatch one console command line.
// Each command either toggles a runtime flag, switches display mode, or prints
// status information for debugging on the device.
static bool handleConsoleCommand(char *line, char *response = nullptr, size_t responseSize = 0) {
    char *cmd = trimLine(line);
    if (cmd[0] == '\0') {
        return true;
    }

    printf("CMD: %s\n", cmd);

    if (strcasecmp(cmd, "help") == 0) {
        printConsoleCommandHelp(response, responseSize);
        return true;
    }
    if (strcasecmp(cmd, "cls") == 0) {
        dispConsoleClear();
        dispConsolePrintf("Console cleared\n");
        appendResponse(response, responseSize, "Console cleared\n");
        return true;
    }
    if (strcasecmp(cmd, "console") == 0) {
        dispShowConsole();
        dispConsolePrintf("Console view\n");
        appendResponse(response, responseSize, "Console view\n");
        return true;
    }
    if (strcasecmp(cmd, "emu") == 0) {
        dispShowEmu();
        printf("Display switched to emulator view\n");
        appendResponse(response, responseSize, "Display switched to emulator view\n");
        return true;
    }
    if (strcasecmp(cmd, "storage") == 0) {
        appendResponse(response, responseSize, "Entering USB mass storage mode...\n");
        bool ok = storageUsbStartMassStorageMode();
        if (!ok) {
            appendResponse(response, responseSize, "USB mass storage mode failed\n");
        }
        return ok;
    }
    if (strcasecmp(cmd, "log on") == 0) {
        appSetConsoleOutEnabled(true);
        printf("Log output enabled\n");
        dispConsolePrintf("Log output on\n");
        appendResponse(response, responseSize, "Log output enabled\n");
        return true;
    }
    if (strcasecmp(cmd, "log off") == 0) {
        appSetConsoleOutEnabled(false);
        printf("Log output disabled\n");
        dispConsolePrintf("Log output off\n");
        appendResponse(response, responseSize, "Log output disabled\n");
        return true;
    }
    if (strcasecmp(cmd, "perf on") == 0) {
        emuMonitorSetDetailedEnabled(true);
        printf("Perf detail enabled\n");
        dispConsolePrintf("Perf detail on\n");
        appendResponse(response, responseSize, "Perf detail enabled\n");
        return true;
    }
    if (strcasecmp(cmd, "perf off") == 0) {
        emuMonitorSetDetailedEnabled(false);
        printf("Perf detail disabled\n");
        dispConsolePrintf("Perf detail off\n");
        appendResponse(response, responseSize, "Perf detail disabled\n");
        return true;
    }
    if (strcasecmp(cmd, "cpu on") == 0) {
        emuMonitorSetCoreUsageEnabled(true);
        printf("CPU load enabled\n");
        dispConsolePrintf("CPU load on\n");
        appendResponse(response, responseSize, "CPU load enabled\n");
        return true;
    }
    if (strcasecmp(cmd, "cpu off") == 0) {
        emuMonitorSetCoreUsageEnabled(false);
        printf("CPU load disabled\n");
        dispConsolePrintf("CPU load off\n");
        appendResponse(response, responseSize, "CPU load disabled\n");
        return true;
    }
    if (strcasecmp(cmd, "turbo on") == 0) {
        appSetTurboEnabled(true);
        printf("Turbo enabled\n");
        dispConsolePrintf("Turbo on\n");
        appendResponse(response, responseSize, "Turbo enabled\n");
        return true;
    }
    if (strcasecmp(cmd, "turbo off") == 0) {
        appSetTurboEnabled(false);
        printf("Turbo disabled\n");
        dispConsolePrintf("Turbo off\n");
        appendResponse(response, responseSize, "Turbo disabled\n");
        return true;
    }
    if (strcasecmp(cmd, "show wifi") == 0) {
        printWifiStatus(response, responseSize);
        return true;
    }
    if (strncasecmp(cmd, "set wifi ", 9) == 0) {
        char *ssid = nullptr;
        char *password = nullptr;
        if (!parseSetWifiArgs(cmd + 9, &ssid, &password)) {
            printf("Usage: set wifi SSID PASSWORD\n");
            dispConsolePrintf("Usage: set wifi SSID PASSWORD\n");
            appendResponse(response, responseSize, "Usage: set wifi SSID PASSWORD\n");
            return false;
        }
        bool ok = appEnsureSdBootstrapFiles() &&
                  appSaveTextSetting("ssid", ssid) &&
                  appSaveTextSetting("password", password);
        if (ok) {
            printf("WiFi setting saved: %s\n", ssid);
            dispConsolePrintf("WiFi setting saved\n");
            appendResponse(response, responseSize, "WiFi setting saved\n");
        } else {
            printf("WiFi setting save failed\n");
            dispConsolePrintf("WiFi setting save failed\n");
            appendResponse(response, responseSize, "WiFi setting save failed\n");
        }
        return ok;
    }
    if (strcasecmp(cmd, "connect wifi") == 0) {
        AppSettings cfg;
        reloadSettings(&cfg);
        printf("WiFi connecting...\n");
        dispConsolePrintf("WiFi connecting...\n");
        bool ok = macHttpReconnectWifi(&cfg);
        if (macHttpStationConnected()) {
            printf("WiFi connected; syncing NTP...\n");
            dispConsolePrintf("WiFi connected\n");
            if (syncRtcFromNtpConfig(&cfg)) {
                printf("NTP sync ok\n");
                appendResponse(response, responseSize, "WiFi connected; NTP sync ok\n");
            } else {
                printf("NTP sync failed\n");
                appendResponse(response, responseSize, "WiFi connected; NTP sync failed\n");
            }
            return true;
        }
        if (ok) {
            printf("WiFi station failed; setup AP active\n");
            dispConsolePrintf("WiFi AP ESP32Mac-Setup\n");
            appendResponse(response, responseSize, "WiFi station failed; setup AP active\n");
        } else {
            printf("WiFi connect failed\n");
            dispConsolePrintf("WiFi connect failed\n");
            appendResponse(response, responseSize, "WiFi connect failed\n");
        }
        return false;
    }
    if (strcasecmp(cmd, "reboot") == 0) {
        if (scheduleRestart()) {
            printf("Rebooting...\n");
            dispConsolePrintf("Rebooting...\n");
            appendResponse(response, responseSize, "Rebooting...\n");
            fflush(stdout);
            return true;
        }
        printf("Failed to schedule reboot\n");
        appendResponse(response, responseSize, "Failed to schedule reboot\n");
        return false;
    }
#if ESP32_MAC_OPCODE_PROFILER
    if (strcasecmp(cmd, "opprof on") == 0) {
        if (m68k_opcode_profiler_set_enabled(1)) {
            dispConsolePrintf("Opcode profiler on\n");
            appendResponse(response, responseSize, "Opcode profiler on\n");
        } else {
            dispConsolePrintf("Opcode profiler alloc failed\n");
            appendResponse(response, responseSize, "Opcode profiler alloc failed\n");
        }
        return true;
    }
    if (strcasecmp(cmd, "opprof off") == 0) {
        m68k_opcode_profiler_set_enabled(0);
        dispConsolePrintf("Opcode profiler off\n");
        appendResponse(response, responseSize, "Opcode profiler off\n");
        return true;
    }
    if (strcasecmp(cmd, "opprof reset") == 0) {
        m68k_opcode_profiler_reset();
        dispConsolePrintf("Opcode profiler reset\n");
        appendResponse(response, responseSize, "Opcode profiler reset\n");
        return true;
    }
    if (strncasecmp(cmd, "opprof dump", 11) == 0) {
        unsigned int limit = 64;
        const char *arg = cmd + 11;
        while (*arg == ' ' || *arg == '\t') {
            ++arg;
        }
        if (*arg != '\0') {
            unsigned long parsed = strtoul(arg, nullptr, 10);
            if (parsed > 0 && parsed < 2048) {
                limit = (unsigned int)parsed;
            }
        }
        m68k_opcode_profiler_dump(limit);
        dispConsolePrintf("Opcode profile dumped\n");
        appendResponse(response, responseSize, "Opcode profile dumped to serial\n");
        return true;
    }
#endif
    if (strcasecmp(cmd, "status") == 0) {
        printConsoleStatus(response, responseSize);
        return true;
    }

    printf("Unknown command: %s\n", cmd);
    dispConsolePrintf("Unknown command:\n%s\n", cmd);
    appendResponse(response, responseSize, "Unknown command\n");
    return false;
}

// Background serial input loop for the lightweight command shell.
static void consoleCommandTask(void *param) {
    (void)param;
    char line[128];
    size_t lineLen = 0;
    bool sawCarriageReturn = false;

    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    // printConsoleCommandHelp();
    printPrompt();
    while (1) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            delayMs(50);
            continue;
        }

        if (ch == '\n' && sawCarriageReturn) {
            sawCarriageReturn = false;
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (lineLen > 0) {
                line[lineLen] = '\0';
                handleConsoleCommand(line);
                lineLen = 0;
            }
            printPrompt();
            sawCarriageReturn = (ch == '\r');
            continue;
        }

        sawCarriageReturn = false;

        if (ch == '\b' || ch == 0x7F) {
            if (lineLen > 0) {
                --lineLen;
            }
            continue;
        }

        if (lineLen + 1 < sizeof(line)) {
            line[lineLen++] = (char)ch;
        }
    }
}

bool consoleShellRunCommand(const char *command, char *response, size_t responseSize) {
    if (!command) {
        return false;
    }
    char line[128];
    snprintf(line, sizeof(line), "%s", command);
    if (response && responseSize > 0) {
        response[0] = '\0';
    }
    return handleConsoleCommand(line, response, responseSize);
}

// Start the serial command shell and register the ROM-source callback it uses.
void consoleShellStart(ConsoleShellRomSourceGetter romSourceGetter) {
    gRomSourceGetter = romSourceGetter;
    BaseType_t ok = xTaskCreatePinnedToCore(consoleCommandTask, "console_cmd", 4096, NULL, 1, NULL, 0);
    if (ok != pdPASS) {
        printf("ERROR: failed to start console_cmd task\n");
    }
}
