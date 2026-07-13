#include "screenshot.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "console_log.h"
#include "tme/vmu.h"

namespace {
constexpr unsigned kWidth = 640;
constexpr unsigned kHeight = 480;
constexpr size_t kRowBytes = kWidth / 8;

uint8_t *sCapture;
volatile bool sBusy;

void put16le(FILE *fp, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    fwrite(bytes, 1, sizeof(bytes), fp);
}

void put32le(FILE *fp, uint32_t value) {
    uint8_t bytes[4] = {(uint8_t)value, (uint8_t)(value >> 8),
                        (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    fwrite(bytes, 1, sizeof(bytes), fp);
}

bool saveBmp(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    constexpr uint32_t pixelBytes = kRowBytes * kHeight;
    constexpr uint32_t fileBytes = 14 + 40 + 8 + pixelBytes;
    put16le(fp, 0x4D42); put32le(fp, fileBytes); put16le(fp, 0); put16le(fp, 0); put32le(fp, 62);
    put32le(fp, 40); put32le(fp, kWidth); put32le(fp, kHeight);
    put16le(fp, 1); put16le(fp, 1); put32le(fp, 0); put32le(fp, pixelBytes);
    put32le(fp, 2835); put32le(fp, 2835); put32le(fp, 2); put32le(fp, 0);
    // Match the LCD's visible polarity: a set framebuffer bit is white.
    // Palette entries are BGRA for a 1bpp BMP.
    const uint8_t palette[] = {255,255,255,0, 0,0,0,0};
    fwrite(palette, 1, sizeof(palette), fp);
    uint8_t row[kRowBytes];
    for (int y = kHeight - 1; y >= 0; --y) {
        // VMU stores the Mac logical framebuffer. LCD rotation and bit order
        // are applied only by the display transfer LUT, not to this surface.
        memcpy(row, sCapture + y * kRowBytes, kRowBytes);
        if (fwrite(row, 1, kRowBytes, fp) != kRowBytes) {
            fclose(fp);
            return false;
        }
    }
    fclose(fp);
    return true;
}

void saveTask(void *) {
    mkdir("/sd/image", 0775);
    unsigned number = 1;
    char path[64];
    struct stat st;
    do {
        snprintf(path, sizeof(path), "/sd/image/img%05u.bmp", number++);
    } while (stat(path, &st) == 0 && number < 100000);

    bool ok = saveBmp(path);
    if (ok) {
        const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
        consoleLogPrintf("%s saved\n", name);
    } else {
        consoleLogPrintf("screenshot save failed\n");
    }
    sBusy = false;
    vTaskDelete(nullptr);
}
} // namespace

bool screenshotRequest(char *response, size_t responseSize) {
    if (sBusy) {
        if (response && responseSize) snprintf(response, responseSize, "screenshot busy\n");
        return false;
    }
    sBusy = true;
    sCapture = vmuCaptureFrontBuffer();
    if (!sCapture) {
        sBusy = false;
        if (response && responseSize) snprintf(response, responseSize, "screenshot buffer alloc failed\n");
        return false;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(saveTask, "screenshot", 4096, nullptr, 2, nullptr, 0);
    if (ok != pdPASS) {
        sBusy = false;
        if (response && responseSize) snprintf(response, responseSize, "screenshot task failed\n");
        return false;
    }
    if (response && responseSize) snprintf(response, responseSize, "screenshot saving\n");
    return true;
}
