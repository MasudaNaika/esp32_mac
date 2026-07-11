/*
 * Mac Plus Emulator on Waveshare ESP32-S3-Touch-LCD-2.8B
 * Main entry point: initializes display, loads ROM from flash, starts emulation.
 */
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_clk.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST7701.h"
#include "app_settings.h"
#include "ble_input.h"
#include "console_shell.h"
#include "http_server.h"
#include "net_time.h"
#include "pv_sony_driver.h"
#include "sdcard.h"
#include "storage_menu.h"
#include "storage_usb.h"

extern "C" {
#include "tme/disp.h"
#include "tme/snd.h"
#include "tme/emu.h"
#include "tme/rtc.h"
#include "tme/tmeconfig.h"
#include "tme/vmu.h"
}

static uint8_t *romData = NULL;
static char pram[32];
static const char *gRomSource = "unknown";
static char gConfiguredRomName[128] = "macplus.rom";
static char gConfiguredHdNames[SCSI_TARGET_COUNT][128] = {
    "", "", "", "", "", "", "hd/hd.img"
};
static char gConfiguredFddNames[PV_FDD_DRIVE_COUNT][128] = {};
static AppSettings gBootCfg;

// Small boot-time sleep helper so startup sequencing stays readable.
static void delayMs(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void copyBootSettingName(char *dst, size_t dstSize, const char *src) {
    if (dstSize == 0) {
        return;
    }
    size_t len = strnlen(src ? src : "", dstSize - 1);
    memcpy(dst, src ? src : "", len);
    dst[len] = '\0';
}

static void logBootHeap(const char *tag) {
    printf("BOOT heap %s: DRAM free=%u largest=%u DMA free=%u largest=%u PSRAM free=%u largest=%u\n",
           tag,
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

static void showRecoveryUrlOrQr(const char *status) {
    char url[64] = {};
    if (macHttpGetActiveUrl(url, sizeof(url)) &&
        dispConsoleShowQrCode("ROM missing - Web setup", url, status)) {
        return;
    }
    dispConsoleClear();
    dispConsolePrintf("ERROR: ROM not available\n");
    if (url[0]) {
        dispConsolePrintf("HTTP %s\n", url);
    }
    dispConsolePrintf("Upload ROM as sd/macplus.rom\n");
    dispConsoleSetStatus("%s", status);
}

static void haltForRomRecovery(AppSettings *bootCfg) {
    printf("\nHalted - ROM not available.\n");
    if (romData) {
        heap_caps_free(romData);
        romData = NULL;
    }
    dispShowConsole();
    dispConsolePrintf("ERROR: ROM not available\n");
    dispConsolePrintf("Starting recovery WiFi...\n");
    logBootHeap("before recovery HTTP");
    bool httpStarted = macHttpServerStart(bootCfg);
    logBootHeap("after recovery HTTP");
    if (httpStarted) {
        dispConsolePrintf("Recovery HTTP ready\n");
        showRecoveryUrlOrQr("Short/Long: menu");
    } else {
        dispConsolePrintf("Recovery HTTP failed\n");
        dispConsoleSetStatus("Short/Long: menu");
    }

    dispSetButtonMenuMode(true);
    while (1) {
        int event = dispWaitButtonEvent(UINT32_MAX);
        if (event == 0) {
            continue;
        }
        while (dispBootButtonPressed()) {
            delayMs(20);
        }
        StorageMenuResult menuResult =
            storageMenuRun(gConfiguredFddNames, gConfiguredHdNames);
        if (menuResult == StorageMenuResult::UsbStorage) {
            storageUsbStartMassStorageMode();
            while (1) {
                delayMs(1000);
            }
        }
        showRecoveryUrlOrQr(httpStarted ? "Short/Long: menu" : "HTTP failed");
    }
}

// --- ROM patching for 640x480 screen (from Mini vMac SCRNHACK.h) ---
// Mac Plus ROM has 512x342 hardcoded. We patch it to 640x480.
#define PATCHED_W 640
#define PATCHED_H 480
#define PATCHED_VIDBASE 0x00540000      // Video memory outside RAM (like Mini vMac)
#define ENABLE_NTP_RTC_SYNC 1

// Write one big-endian 16-bit value into the ROM patch buffer.
static inline void rom_put_word(uint8_t *rom, int offset, uint16_t val) {
    rom[offset]     = (val >> 8) & 0xFF;
    rom[offset + 1] = val & 0xFF;
}

// Write one big-endian 32-bit value into the ROM patch buffer.
static inline void rom_put_long(uint8_t *rom, int offset, uint32_t val) {
    rom[offset]     = (val >> 24) & 0xFF;
    rom[offset + 1] = (val >> 16) & 0xFF;
    rom[offset + 2] = (val >> 8) & 0xFF;
    rom[offset + 3] = val & 0xFF;
}

// Rewrite the stock Mac Plus ROM constants for the 640x480 LCD layout.
// Steps:
// 1. disable checksum and slow RAM-test behavior,
// 2. move screen base references into our external VRAM window,
// 3. patch QuickDraw, icons, cursor limits, and screen records.
static void patchRomForLargeScreen(uint8_t *rom) {
    const int W = PATCHED_W;
    const int H = PATCHED_H;
    const uint32_t VB = PATCHED_VIDBASE;

    printf("ROM PATCH: %dx%d, stride=%d, vidbase=0x%06lX\n", W, H, W/8, (unsigned long)VB);

    // 1. Disable ROM checksum (patches invalidate it)
    rom_put_word(rom, 0xD7A, 0x6022);  // BRA +0x22 (skip checksum)

    // 2. Disable RAM test for faster boot
    rom_put_word(rom, 3752, 0x4E71);  // NOP
    rom_put_word(rom, 3728, 0x4E71);  // NOP

    // 3. Video memory base references
    rom_put_long(rom, 138, VB);
    rom_put_long(rom, 326, VB);

    // 4. Sad mac error display positions
    rom_put_long(rom, 356, VB + (((H/4)*2+9)*W + (W/2-24))/8);
    rom_put_word(rom, 392, W / 8);
    rom_put_word(rom, 404, W / 8);
    rom_put_word(rom, 412, W / 4 * 3 - 1);
    rom_put_long(rom, 420, VB + (((H/4)*2+17)*W + (W/2-8))/8);

    // 5. Screen clear: size in longwords
    rom_put_word(rom, 494, (H * W / 32) - 1);

    // 6. Screen setup: JSR to a patch that sets ScrnBase.
    // The stock .Sony resource is always replaced, so its unused tail is safe.
    constexpr int patchAddr = 0x17F20;
    // At ROM offset 1132: JSR to our patch
    rom_put_word(rom, 1132, 0x4EB9);                   // JSR abs.L
    rom_put_long(rom, 1134, 0x00400000 + patchAddr);    // ROM base + patch offset
    // Patch routine: MOVE.L #VB, (ScrnBase=0x0824)
    rom_put_word(rom, patchAddr,     0x21FC);           // MOVE.L #imm, (abs).W
    rom_put_long(rom, patchAddr + 2, VB);               // immediate value
    rom_put_word(rom, patchAddr + 6, 0x0824);           // ScrnBase low-mem global
    rom_put_word(rom, patchAddr + 8, 0x4E75);           // RTS

    // 7. QuickDraw screen bitmap dimensions
    rom_put_word(rom, 1140, W / 8);   // screenRow (row bytes)
    rom_put_word(rom, 1172, H);       // screen height
    rom_put_word(rom, 1176, W);       // screen width

    // 8. Floppy blink icon positions
    rom_put_long(rom, 2016, VB + (((H/4)*2-25)*W + (W/2-16))/8);
    rom_put_long(rom, 2034, VB + (((H/4)*2-10)*W + (W/2-8))/8);

    // 9. Additional screen dimension references
    rom_put_word(rom, 2574, H);
    rom_put_word(rom, 2576, W);

    // 10. Sad mac icon positions
    rom_put_word(rom, 3810, W / 8 - 4);
    rom_put_word(rom, 3826, W / 8);
    rom_put_long(rom, 3852, VB + (((H/4)*2-25)*W + (W/2-16))/8);
    rom_put_long(rom, 3864, VB + (((H/4)*2-19)*W + (W/2-8))/8);
    rom_put_word(rom, 3894, W / 8 - 2);

    // 11. Cursor handling (W < 1024, so MOVEQ works)
    rom_put_word(rom, 7376, 0x7000 + (W/8));  // MOVEQ #(W/8), D0
    rom_put_word(rom, 7496, W - 32);           // cursor X clamp
    rom_put_word(rom, 7502, W - 32);           // cursor X clamp
    rom_put_word(rom, 7534, H - 16);           // cursor Y clamp
    rom_put_word(rom, 7540, H);                // cursor Y limit
    rom_put_word(rom, 7570, 0x7A00 + (W/8));   // MOVEQ #(W/8), D5

    // 12. screenBits record
    rom_put_word(rom, 7784, H);   // screenBits.bounds.bottom
    rom_put_word(rom, 7790, W);   // screenBits.bounds.right
    rom_put_word(rom, 7810, H);   // screen height

    if (!pvSonyPatchRom(rom, TME_ROMSIZE)) {
        printf("FDD ROM PATCH: .Sony layout is unsupported\n");
        memset(gConfiguredFddNames, 0, sizeof(gConfiguredFddNames));
    }

    printf("ROM PATCH: Applied patches for %dx%d screen\n", W, H);
}

// Called by rtc.c when PRAM changes — save to NVS
// Persist the 32-byte PRAM/RTC blob whenever the RTC model changes it.
extern "C" void saveRtcMem(char *mem) {
    nvs_handle_t handle;
    if (nvs_open("macplus", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, "pram", mem, 32);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

// Load the persisted PRAM/RTC blob from NVS before the ROM starts.
static void loadPram() {
    memset(pram, 0, sizeof(pram));
    nvs_handle_t handle;
    if (nvs_open("macplus", NVS_READONLY, &handle) == ESP_OK) {
        size_t len = 32;
        if (nvs_get_blob(handle, "pram", pram, &len) == ESP_OK) {
            printf("PRAM loaded from NVS (%d bytes)\n", (int)len);
        } else {
            printf("No PRAM in NVS, using defaults\n");
        }
        nvs_close(handle);
    }
}

// Try to load the ROM image from the SD card.
// Steps:
// 1. resolve the configured ROM path,
// 2. allocate the ROM buffer in PSRAM,
// 3. read the fixed-size ROM image and record the source.
static bool loadRomFromSD() {
    if (!sdcardMounted()) return false;

    char romPath[160];
    const char *resolvedRomPath = resolveSdConfigPath(gConfiguredRomName, "macplus.rom", romPath, sizeof(romPath));
    FILE *fp = fopen(resolvedRomPath, "rb");
    if (!fp) {
        printf("ROM: %s not found\n", resolvedRomPath);
        return false;
    }

    romData = (uint8_t*)tme_psram_aligned_alloc(TME_ROMSIZE, MALLOC_CAP_SPIRAM);
    if (!romData) {
        fclose(fp);
        printf("ERROR: Could not allocate ROM buffer\n");
        return false;
    }

    size_t read = fread(romData, 1, TME_ROMSIZE, fp);
    fclose(fp);
    printf("ROM: Loaded %d bytes from %s\n", (int)read, resolvedRomPath);
    gRomSource = "sd";
    return true;
}

// Try to load the ROM image from the fallback flash partition.
// This keeps the machine bootable when no SD card is present.
static bool loadRomFromFlash() {
    const esp_partition_t *part = esp_partition_find_first(
        (esp_partition_type_t)0x40, (esp_partition_subtype_t)0x01, NULL);
    if (!part) {
        printf("ROM: Flash partition not found\n");
        return false;
    }
    printf("ROM partition found: offset=0x%lx size=%ld\n", part->address, part->size);

    romData = (uint8_t*)tme_psram_aligned_alloc(TME_ROMSIZE, MALLOC_CAP_SPIRAM);
    if (!romData) {
        printf("ERROR: Could not allocate ROM buffer\n");
        return false;
    }

    esp_err_t err = esp_partition_read(part, 0, romData, TME_ROMSIZE);
    if (err != ESP_OK) {
        printf("ERROR: Could not read ROM: %s\n", esp_err_to_name(err));
        return false;
    }
    printf("ROM: Loaded from flash partition\n");
    gRomSource = "flash";
    return true;
}

// Load and validate the ROM image used by the emulator core.
// Steps:
// 1. prefer SD, then fall back to flash,
// 2. inspect the first bytes and reset vector,
// 3. reject obviously blank or invalid images.
static bool loadRom() {
    if (!loadRomFromSD() && !loadRomFromFlash()) {
        printf("ERROR: No ROM available!\n");
        printf("Either place macplus.rom on SD card or flash it:\n");
        printf("  esptool.py write_flash 0x190000 macplus.rom\n");
        return false;
    }

    // Validate: 68K reset vector
    // Sanity-check the reset vector before we hand the ROM to the CPU core.
    uint32_t initPC = ((uint32_t)romData[4] << 24) | ((uint32_t)romData[5] << 16) |
                      ((uint32_t)romData[6] << 8)  | (uint32_t)romData[7];
    printf("ROM loaded. First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
           romData[0], romData[1], romData[2], romData[3],
           romData[4], romData[5], romData[6], romData[7]);
    printf("Initial PC: 0x%08lX\n", (unsigned long)initPC);

    // A quick sanity check catches blank or obviously wrong ROM images early.
    if (romData[0] == 0xFF && romData[1] == 0xFF) {
        printf("ERROR: ROM appears to be blank (0xFF).\n");
        return false;
    }
    if (initPC > 0x100000 && (initPC < 0x400000 || initPC >= 0x500000)) {
        printf("ERROR: Initial PC 0x%08lX is invalid — not a Mac Plus ROM.\n", (unsigned long)initPC);
        return false;
    }
    return true;
}

// FreeRTOS task wrapper that hands control to the emulator loop on core 1.
static void emuTask(void *param) {
    (void)param;

    // The emulator loop owns the Mac hardware model and never returns.
    printf("Starting Mac Plus emulation on core %d...\n", xPortGetCoreID());
    const char *hdNames[SCSI_TARGET_COUNT];
    for (int id = 0; id < SCSI_TARGET_COUNT; ++id) hdNames[id] = gConfiguredHdNames[id];
    const char *fddNames[PV_FDD_DRIVE_COUNT];
    for (int drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
        fddNames[drive] = gConfiguredFddNames[drive];
    }
    tmeStartEmu(romData, hdNames, fddNames);
    // tmeStartEmu never returns
    vTaskDelete(NULL);
}

// Report the current ROM source string to the shell and status UI.
static const char *romSource() {
    return gRomSource;
}

// Main ESP-IDF entry point.
// Steps:
// 1. initialize persistence and board peripherals,
// 2. parse settings and bring up the boot console,
// 3. restore PRAM, load and patch the ROM, optionally sync NTP,
// 4. initialize BLE, print memory stats, and launch the emulator task.
extern "C" void app_main(void) {
    delayMs(2000);
    printf("\n\n=== Mac Plus Emulator ===\n");
    printf("ESP32-S3 + Waveshare 2.8\" ST7701 LCD\n\n");

    // Init NVS before PRAM or settings persistence is touched.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Bring up just enough board hardware to show the boot console.
    I2C_Init();
    TCA9554PWR_Init(0x00);
    Set_EXIO(EXIO_PIN8, Low);
   
    // Read the boot config before the display and ROM paths lock in.
    AppSettings &bootCfg = gBootCfg;
    appSettingsInitDefaults(&bootCfg);
    if (sdcardInit()) {
        appEnsureSdBootstrapFiles();
        // Parse settings once before display/ROM setup so later stages share
        // the same ROM, HD, logging, and orientation choices.
        loadSettingsFromSd(&bootCfg);
        sdcardDeinit();
    }
    appSetConsoleOutEnabled(bootCfg.consoleOut);
    appSetTurboEnabled(bootCfg.turbo);
    appSetAudioBackend(bootCfg.audioBackend);
    appSetBootInfoEnabled(bootCfg.bootInfo);
    copyBootSettingName(gConfiguredRomName, sizeof(gConfiguredRomName), bootCfg.rom);
    for (int id = 0; id < SCSI_TARGET_COUNT; ++id) {
        copyBootSettingName(gConfiguredHdNames[id], sizeof(gConfiguredHdNames[id]), bootCfg.hd[id]);
    }
    for (int drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
        copyBootSettingName(gConfiguredFddNames[drive], sizeof(gConfiguredFddNames[drive]),
                            bootCfg.fd[drive]);
    }

    Backlight_Init();
    Set_Backlight(100);
    vmuInit();
    LCD_SetMacFrameBuffer(vmuGetFrontBuffer(), bootCfg.displayRotate180);
    LCD_Init();
    printf("Display initialized.\n");
    dispConsoleInit();
    // Route BOOT gestures to the startup selector while its entry window is open.
    dispSetButtonMenuMode(true);
    dispSetRotate180(bootCfg.displayRotate180);
    const bool bootInfo = appBootInfoEnabled();
    if (!bootInfo) {
        dispShowEmu();
    }
    // The boot console explains the machine state before the emulator takes over.
    dispConsolePrintf("ESP32-S3 Xtensa LX7 Dual Core CPU %dMHz\n", esp_clk_cpu_freq() / 1000000);
    dispConsolePrintf("Display initialized\n");

    // Free LCD SPI bus so GPIO1/GPIO2 can be reused for SD card
    LCD_FreeSPI();
    dispConsolePrintf("LCD SPI bus released\n");

    // Init SD card (uses GPIO1=CMD, GPIO2=CLK, GPIO42=D0)
    sdcardInit();
    dispConsolePrintf("SD init done\n");
    logBootHeap("after sdcardInit");

    if (bootInfo) {
        dispConsoleSetStatus("Press BOOT for disk menu (3 sec)");
        bool openStorageMenu = dispWaitButtonEvent(3000) != 0;
        if (openStorageMenu) {
            while (dispBootButtonPressed()) {
                delayMs(20);
            }
            // Discard the gesture used to enter the menu before accepting choices.
            dispSetButtonMenuMode(false);
            dispSetButtonMenuMode(true);
            StorageMenuResult menuResult =
                storageMenuRun(gConfiguredFddNames, gConfiguredHdNames);
            if (menuResult == StorageMenuResult::UsbStorage) {
                storageUsbStartMassStorageMode();
                while (1) {
                    delayMs(1000);
                }
            }
        }
    }
    dispSetButtonMenuMode(false);

    // Restore persistent Mac state before the ROM starts executing.
    loadPram();
    rtcInit(pram);
    dispConsolePrintf("PRAM loaded\n");
    if (!loadRom()) {
        haltForRomRecovery(&bootCfg);
        return;
    }
    dispConsolePrintf("ROM loaded %s\n", gRomSource);

    // Apply the widescreen ROM hack after the image source has been finalized.
    patchRomForLargeScreen(romData);
    dispConsolePrintf("ROM patched for 640x480 LCD\n");
    for (int drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
        if (gConfiguredFddNames[drive][0] != '\0') {
            dispConsolePrintf("FD%d image %s\n", drive, gConfiguredFddNames[drive]);
        }
    }

    bool networkStarted = false;
    if (bootCfg.ssid[0]) {
        dispConsolePrintf("WiFi SSID %s\n", bootCfg.ssid);
    } else {
        dispConsolePrintf("WiFi setup mode\n");
    }
    dispConsolePrintf("WiFi starting...\n");
    logBootHeap("before macHttpNetworkStart");
    networkStarted = macHttpNetworkStart(&bootCfg);
    logBootHeap("after macHttpNetworkStart");
    if (macHttpStationConnected()) {
        dispConsolePrintf("WiFi connected\n");
    } else if (networkStarted) {
        dispConsolePrintf("WiFi AP ESP32Mac-Setup\n");
        dispConsolePrintf("HTTP http://192.168.6.1/\n");
    } else {
        dispConsolePrintf("WiFi failed\n");
    }

    if (ENABLE_NTP_RTC_SYNC) {
        // NTP is optional; when enabled it keeps the host and Mac RTC fresh.
        dispConsolePrintf("NTP sync 60min...\n");
        if (macHttpStationConnected()) {
            dispConsolePrintf("NTP %s tz %+ldh\n",
                              bootCfg.server,
                              (long)(bootCfg.tzOffsetSeconds / 3600));
        }
        if (macHttpStationConnected() && startPeriodicNtpSync(&bootCfg)) {
            dispConsolePrintf("NTP periodic started\n");
        } else {
            dispConsolePrintf("NTP skipped/failed\n");
        }
    } else {
        printf("NTP: RTC sync disabled\n");
        dispConsolePrintf("NTP disabled\n");
    }

    if (networkStarted) {
        dispConsolePrintf("HTTP server...\n");
        logBootHeap("before macHttpServerStart");
        if (macHttpServerStart(&bootCfg)) {
            dispConsolePrintf("HTTP http://esp32mac.local/\n");
        } else {
            dispConsolePrintf("HTTP failed\n");
        }
        logBootHeap("after macHttpServerStart");
    } else {
        dispConsolePrintf("HTTP skipped (no WiFi)\n");
    }

    // BLE input is ready before the emulator starts so the Mac can consume it
    // as soon as the ROM begins polling devices.
    logBootHeap("before bleInputInit");
    bleInputInit();
    logBootHeap("after bleInputInit");
    dispConsolePrintf("BLE initialized\n");

    // Report allocator state so boot-time diagnostics are visible on-device.
    int freeMem, largestMem;
    // DRAM
    const char *msg1 = "DRAM free=%d, largest block=%d\n";
    freeMem = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    largestMem = (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    printf(msg1, freeMem, largestMem);
    dispConsolePrintf(msg1, freeMem, largestMem);
    // DMA
    const char *msg2 = "DMA free=%d, largest block=%d\n";
    freeMem = (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    largestMem = (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    printf(msg2, freeMem, largestMem);
    dispConsolePrintf(msg2, freeMem, largestMem);
    // PSRAM
    const char *msg3 = "PSRAM free=%d, largest block=%d\n";
    freeMem = (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    largestMem = (int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    printf(msg3, freeMem, largestMem);
    dispConsolePrintf(msg3 , freeMem, largestMem);

    // force sound isr on core 0
    logBootHeap("before sndInit");
	sndInit();
	logBootHeap("after sndInit");

    // Core 1 runs the emulator while core 0 keeps the console/display path live.
    dispConsolePrintf("Starting Mac Plus Emulator on ESP32-S3\n");
    xTaskCreatePinnedToCore(emuTask, "emu", 8192, NULL, 5, NULL, 1);
    storageMenuStartRuntime(gConfiguredFddNames, gConfiguredHdNames);
    consoleShellStart(romSource);
}
