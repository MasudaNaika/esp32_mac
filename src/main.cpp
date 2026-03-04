/*
 * Mac Plus Emulator on Waveshare ESP32-S3-Touch-LCD-2.8B
 * Main entry point: initializes display, loads ROM from flash, starts emulation.
 */
#include <Arduino.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST7701.h"

extern "C" {
#include "tme/emu.h"
#include "tme/rtc.h"
#include "tme/tmeconfig.h"
}

static uint8_t *romData = NULL;
static char pram[32];

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

static bool loadRom() {
    const esp_partition_t *part = esp_partition_find_first(
        (esp_partition_type_t)0x40, (esp_partition_subtype_t)0x01, NULL);
    if (!part) {
        printf("ERROR: ROM partition not found!\n");
        printf("Flash ROM with: esptool.py write_flash 0x110000 macplus.rom\n");
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

    // Validate: 68K reset vector. Bytes 4-7 = initial PC, should point into ROM area.
    uint32_t initPC = ((uint32_t)romData[4] << 24) | ((uint32_t)romData[5] << 16) |
                      ((uint32_t)romData[6] << 8)  | (uint32_t)romData[7];
    printf("ROM loaded. First 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
           romData[0], romData[1], romData[2], romData[3],
           romData[4], romData[5], romData[6], romData[7]);
    printf("Initial PC: 0x%08lX\n", (unsigned long)initPC);

    if (romData[0] == 0xFF && romData[1] == 0xFF) {
        printf("ERROR: ROM appears to be blank (0xFF). Flash it first!\n");
        printf("  esptool.py write_flash 0x110000 macplus.rom\n");
        return false;
    }
    // Valid Mac Plus ROM has initial PC in low ROM area (remapped to 0x000000)
    // typically around 0x0000xxxx range
    if (initPC > 0x100000 && (initPC < 0x400000 || initPC >= 0x500000)) {
        printf("ERROR: Initial PC 0x%08lX is invalid — not a Mac Plus ROM.\n", (unsigned long)initPC);
        printf("Flash a valid ROM with:\n");
        printf("  esptool.py write_flash 0x110000 macplus.rom\n");
        return false;
    }
    return true;
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

    // Start emulation on core 0 (core 1 handles display refresh from emu)
    xTaskCreatePinnedToCore(emuTask, "emu", 8192, NULL, 5, NULL, 0);
}

void loop() {
    // Nothing — emulation runs in its own task
    delay(1000);
}
