/*
 * Mac Plus Emulator on Waveshare ESP32-S3-Touch-LCD-2.8B
 * Main entry point: initializes display, loads ROM from flash, starts emulation.
 */
#include <Arduino.h>
#include <string.h>
#include <WiFiManager.h>
#include <ESPmDNS.h>
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST7701.h"
#include "wifi_input.h"
#include "sdcard.h"

extern "C" {
#include "tme/emu.h"
#include "tme/rtc.h"
#include "tme/tmeconfig.h"
}

// Defined in disp.cpp
extern void dispShowMessage(const char *lines[], int nlines);

static uint8_t *romData = NULL;
static char pram[32];

// --- ROM patching for 640x480 screen (from Mini vMac SCRNHACK.h) ---
// Mac Plus ROM has 512x342 hardcoded. We patch it to 640x480.
#define PATCHED_W 640
#define PATCHED_H 480
#define PATCHED_STRIDE (PATCHED_W / 8)  // 80
#define PATCHED_VIDBASE 0x00540000      // Video memory outside RAM (like Mini vMac)

static inline void rom_put_word(uint8_t *rom, int offset, uint16_t val) {
    rom[offset]     = (val >> 8) & 0xFF;
    rom[offset + 1] = val & 0xFF;
}

static inline void rom_put_long(uint8_t *rom, int offset, uint32_t val) {
    rom[offset]     = (val >> 24) & 0xFF;
    rom[offset + 1] = (val >> 16) & 0xFF;
    rom[offset + 2] = (val >> 8) & 0xFF;
    rom[offset + 3] = val & 0xFF;
}

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

    // 6. Screen setup: JSR to patch that sets ScrnBase
    //    We write a small patch routine at the end of the Sony driver area.
    //    Find free space after offset 0x1A000 (well within 128KB ROM).
    int patchAddr = 0x1E000;  // Free area near end of ROM
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

    printf("ROM PATCH: Applied %d patches for %dx%d screen\n", 30, W, H);
}

// Called by rtc.c when PRAM changes — save to NVS
extern "C" void saveRtcMem(char *mem) {
    nvs_handle_t handle;
    if (nvs_open("macplus", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, "pram", mem, 32);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

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

static bool loadRomFromSD() {
    if (!sdcardMounted()) return false;

    FILE *fp = fopen("/sd/macplus.rom", "rb");
    if (!fp) {
        printf("ROM: /sd/macplus.rom not found\n");
        return false;
    }

    romData = (uint8_t*)heap_caps_malloc(TME_ROMSIZE, MALLOC_CAP_SPIRAM);
    if (!romData) {
        fclose(fp);
        printf("ERROR: Could not allocate ROM buffer\n");
        return false;
    }

    size_t read = fread(romData, 1, TME_ROMSIZE, fp);
    fclose(fp);
    printf("ROM: Loaded %d bytes from /sd/macplus.rom\n", (int)read);
    return true;
}

static bool loadRomFromFlash() {
    const esp_partition_t *part = esp_partition_find_first(
        (esp_partition_type_t)0x40, (esp_partition_subtype_t)0x01, NULL);
    if (!part) {
        printf("ROM: Flash partition not found\n");
        return false;
    }
    printf("ROM partition found: offset=0x%lx size=%ld\n", part->address, part->size);

    romData = (uint8_t*)heap_caps_malloc(TME_ROMSIZE, MALLOC_CAP_SPIRAM);
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
    return true;
}

static bool loadRom() {
    // Try SD card first, then flash partition
    if (!loadRomFromSD() && !loadRomFromFlash()) {
        printf("ERROR: No ROM available!\n");
        printf("Either place macplus.rom on SD card or flash it:\n");
        printf("  esptool.py write_flash 0x190000 macplus.rom\n");
        return false;
    }

    // Validate: 68K reset vector
    uint32_t initPC = ((uint32_t)romData[4] << 24) | ((uint32_t)romData[5] << 16) |
                      ((uint32_t)romData[6] << 8)  | (uint32_t)romData[7];
    printf("ROM loaded. First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
           romData[0], romData[1], romData[2], romData[3],
           romData[4], romData[5], romData[6], romData[7]);
    printf("Initial PC: 0x%08lX\n", (unsigned long)initPC);

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

void wifiReset() {
    printf("WiFi: erasing saved credentials and restarting...\n");
    WiFiManager wm;
    wm.resetSettings();
    delay(500);
    ESP.restart();
}

static void wifiTask(void *param) {
    WiFiManager wm;
    wm.setConfigPortalTimeout(120);
    wm.setConnectTimeout(15);

    wm.setAPCallback([](WiFiManager *wm) {
        const char *msg[] = {
            "WiFi Setup",
            "",
            "Connect to AP:",
            "MacPlus-Setup",
            "",
            "Then open:",
            "192.168.4.1",
        };
        dispShowMessage(msg, 7);
    });

    if (wm.autoConnect("MacPlus-Setup")) {
        printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
        if (MDNS.begin("macplus")) {
            MDNS.addService("http", "tcp", 80);
            printf("mDNS: macplus.local\n");
        }
    } else {
        printf("WiFi: portal timed out, continuing without network\n");
    }
    wifiInputInit();
    vTaskDelete(NULL);
}

static void emuTask(void *param) {

    printf("Starting Mac Plus emulation on core %d...\n", xPortGetCoreID());
    tmeStartEmu(romData);
    // tmeStartEmu never returns
    vTaskDelete(NULL);
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    printf("\n\n=== Mac Plus Emulator ===\n");
    printf("ESP32-S3 + Waveshare 2.8\" ST7701 LCD\n\n");

    // Init NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Init I2C + IO expander + display
    I2C_Init();
    TCA9554PWR_Init(0x00);
    Set_EXIO(EXIO_PIN8, Low);
    Backlight_Init();
    Set_Backlight(100);
    LCD_Init();
    printf("Display initialized.\n");

    const char *bootMsg[] = { "Mac Plus", "Starting..." };
    dispShowMessage(bootMsg, 2);

    // Free LCD SPI bus so GPIO1/GPIO2 can be reused for SD card
    LCD_FreeSPI();

    // Init SD card (uses GPIO1=CMD, GPIO2=CLK, GPIO42=D0)
    sdcardInit();

    // Report memory
    printf("Free heap: %d bytes\n", (int)esp_get_free_heap_size());
    printf("Free PSRAM: %d bytes\n", (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Load PRAM and ROM
    loadPram();
    rtcInit(pram);

    if (!loadRom()) {
        printf("\nHalted — ROM not available.\n");
        while (1) delay(1000);
    }

    // Patch ROM for 640x480 screen resolution
    patchRomForLargeScreen(romData);

    // Start emulation on core 0 (core 1 handles display refresh from emu)
    xTaskCreatePinnedToCore(emuTask, "emu", 8192, NULL, 5, NULL, 0);

    // WiFi setup in its own task (autoConnect blocks, must not starve display)
    xTaskCreatePinnedToCore(wifiTask, "wifi", 8192, NULL, 1, NULL, 1);
}

void loop() {
    // Check serial for commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd == "wifireset") {
            wifiReset();
        } else if (cmd == "help") {
            printf("Commands: wifireset, help\n");
        }
    }
    delay(100);
}
