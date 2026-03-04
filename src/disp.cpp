/*
 * Display driver for Mac Plus emulator on Waveshare ESP32-S3-Touch-LCD-2.8B
 *
 * LCD panel: 480x640 portrait. Mac framebuffer: 640x480 landscape (patched ROM).
 * Rotated 90° CW: Mac image is 480 wide x 640 tall — fills the entire display.
 *
 * Runs on core 1 as a separate FreeRTOS task, so display rendering doesn't
 * block the emulation running on core 0.
 */
#include <string.h>
#include <stdio.h>
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Display_ST7701.h"

extern "C" {
#include "tme/disp.h"
}

#define MAC_FB_W 640
#define MAC_FB_H 480
#define MAC_FB_STRIDE (MAC_FB_W / 8)  // 80

#define DISP_W ESP_PANEL_LCD_WIDTH   // 480
#define DISP_H ESP_PANEL_LCD_HEIGHT  // 640

// After 90° CW rotation: Mac image is 480 wide x 640 tall — exact fit
#define IMG_W MAC_FB_H  // 480
#define IMG_H MAC_FB_W  // 640

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF

// Lookup table: byte → 8 RGB565 pixels (expand 1-bit to 16-bit)
static uint16_t lut[256][8];

// Chunk buffer in internal SRAM: 8 rows x 480 cols x 2 bytes = 7680 bytes
#define CHUNK_ROWS 8
static uint16_t chunkBuf[CHUNK_ROWS * IMG_W];

// Display task state
static volatile uint8_t *dispFb = NULL;
static volatile bool dispReady = false;

static void buildLut() {
    for (int b = 0; b < 256; b++) {
        for (int bit = 0; bit < 8; bit++) {
            lut[b][bit] = (b & (0x80 >> bit)) ? COLOR_BLACK : COLOR_WHITE;
        }
    }
}

static void dispTaskFunc(void *param) {
    printf("DISP: task running on core %d\n", xPortGetCoreID());

    while (1) {
        if (!dispFb || !panel_handle) {
            vTaskDelay(10);
            continue;
        }

        const uint8_t *mem = (const uint8_t *)dispFb;

        for (int chunk = 0; chunk < IMG_H; chunk += CHUNK_ROWS) {
            int byteIdx = chunk >> 3;

            for (int col = 0; col < IMG_W; col++) {
                int my = (MAC_FB_H - 1) - col;
                uint8_t byte = mem[my * MAC_FB_STRIDE + byteIdx];
                const uint16_t *expanded = lut[byte];

                chunkBuf[0 * IMG_W + col] = expanded[0];
                chunkBuf[1 * IMG_W + col] = expanded[1];
                chunkBuf[2 * IMG_W + col] = expanded[2];
                chunkBuf[3 * IMG_W + col] = expanded[3];
                chunkBuf[4 * IMG_W + col] = expanded[4];
                chunkBuf[5 * IMG_W + col] = expanded[5];
                chunkBuf[6 * IMG_W + col] = expanded[6];
                chunkBuf[7 * IMG_W + col] = expanded[7];
            }

            esp_lcd_panel_draw_bitmap(panel_handle, 0, chunk,
                                      IMG_W, chunk + CHUNK_ROWS,
                                      chunkBuf);
        }

        vTaskDelay(1);  // ~60fps max, yield between frames
    }
}

void dispInit() {
    printf("DISP: 1:1 rotated %dx%d (full screen), chunk buf %d bytes\n",
           IMG_W, IMG_H, (int)sizeof(chunkBuf));
    buildLut();

    // Start display rendering task on core 1
    xTaskCreatePinnedToCore(dispTaskFunc, "disp", 4096, NULL, 3, NULL, 1);
}

void dispDraw(uint8_t *mem) {
    // Just update the pointer — the display task on core 1 renders continuously
    dispFb = mem;
}
