/*
 * Display driver for Mac Plus emulator on Waveshare ESP32-S3-Touch-LCD-2.8B
 *
 * LCD panel: 480x640 portrait. Mac framebuffer: 512x342 landscape.
 * Rotated 90° CW: Mac image is 342 wide x 512 tall, centered with black bars.
 */
#include <string.h>
#include <stdio.h>
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "Display_ST7701.h"

extern "C" {
#include "tme/disp.h"
}

#define MAC_FB_W 512
#define MAC_FB_H 342
#define MAC_FB_STRIDE (MAC_FB_W / 8)

#define DISP_W ESP_PANEL_LCD_WIDTH   // 480
#define DISP_H ESP_PANEL_LCD_HEIGHT  // 640

// After 90° CW rotation: Mac image is 342 wide x 512 tall
#define H_OFFSET ((DISP_W - MAC_FB_H) / 2)  // 69
#define V_OFFSET ((DISP_H - MAC_FB_W) / 2)  // 64

#define IMG_W MAC_FB_H  // 342
#define IMG_H MAC_FB_W  // 512

#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF

// Chunk buffer in internal SRAM: 8 rows x 342 cols x 2 bytes = 5472 bytes
#define CHUNK_ROWS 8
static uint16_t chunkBuf[CHUNK_ROWS * IMG_W];

void dispInit() {
    printf("DISP: 1:1 rotated %dx%d, chunk buf %d bytes\n",
           IMG_W, IMG_H, (int)sizeof(chunkBuf));

    // Clear display to black
    uint16_t black[DISP_W];
    memset(black, 0, sizeof(black));
    for (int y = 0; y < DISP_H; y++) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, DISP_W, y + 1, black);
    }
}

void dispDraw(uint8_t *mem) {
    if (!mem || !panel_handle) return;

    for (int chunk = 0; chunk < IMG_H; chunk += CHUNK_ROWS) {
        int byteIdx = chunk >> 3;

        for (int col = 0; col < IMG_W; col++) {
            int my = (MAC_FB_H - 1) - col;
            uint8_t byte = mem[my * MAC_FB_STRIDE + byteIdx];

            chunkBuf[0 * IMG_W + col] = (byte & 0x80) ? COLOR_BLACK : COLOR_WHITE;
            chunkBuf[1 * IMG_W + col] = (byte & 0x40) ? COLOR_BLACK : COLOR_WHITE;
            chunkBuf[2 * IMG_W + col] = (byte & 0x20) ? COLOR_BLACK : COLOR_WHITE;
            chunkBuf[3 * IMG_W + col] = (byte & 0x10) ? COLOR_BLACK : COLOR_WHITE;
            chunkBuf[4 * IMG_W + col] = (byte & 0x08) ? COLOR_BLACK : COLOR_WHITE;
            chunkBuf[5 * IMG_W + col] = (byte & 0x04) ? COLOR_BLACK : COLOR_WHITE;
            chunkBuf[6 * IMG_W + col] = (byte & 0x02) ? COLOR_BLACK : COLOR_WHITE;
            chunkBuf[7 * IMG_W + col] = (byte & 0x01) ? COLOR_BLACK : COLOR_WHITE;
        }

        esp_lcd_panel_draw_bitmap(panel_handle, H_OFFSET, V_OFFSET + chunk,
                                  H_OFFSET + IMG_W, V_OFFSET + chunk + CHUNK_ROWS,
                                  chunkBuf);
    }
}
