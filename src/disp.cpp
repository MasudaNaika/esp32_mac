/*
 * Display driver for Mac Plus emulator on Waveshare ESP32-S3-Touch-LCD-2.8B
 *
 * LCD panel: 480x640 portrait. Mac framebuffer: 640x480 landscape (patched ROM).
 * Rotated 90 degrees CW: Mac image is 480 wide x 640 tall and fills the display.
 *
 * Runs a FreeRTOS task for boot-console drawing. The emulated screen is scanned
 * directly from the VMU front buffer by the LCD bounce callback.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "console_font.h"
#include "Display_ST7701.h"
#include "qrcodegen.h"

extern "C" {
#include "tme/disp.h"
#include "tme/vmu.h"
}

#define MAC_FB_W 640
#define MAC_FB_H 480
#define MAC_FB_STRIDE (MAC_FB_W / 8)  // 80

#define DISP_W ESP_PANEL_LCD_WIDTH   // 480
#define DISP_H ESP_PANEL_LCD_HEIGHT  // 640

// After 90 degrees CW rotation: Mac image is 480 wide x 640 tall, exact fit.
#define IMG_W MAC_FB_H  // 480
#define IMG_H MAC_FB_W  // 640

#define FRAMEBUFFER_SIZE (MAC_FB_STRIDE * MAC_FB_H)

#define CONSOLE_LOG_W DISP_H
#define CONSOLE_LOG_H DISP_W
#define CONSOLE_CHAR_W 12
#define CONSOLE_CHAR_H 24
#define CONSOLE_FONT_SCALE_X 2
#define CONSOLE_FONT_SCALE_Y 3
#define CONSOLE_COLS (CONSOLE_LOG_W / CONSOLE_CHAR_W)
#define CONSOLE_ROWS (CONSOLE_LOG_H / CONSOLE_CHAR_H)
#define CONSOLE_STATUS_ROW (CONSOLE_ROWS - 1)
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define CONSOLE_RENDER_POLL_MS 33
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_LONG_PRESS_MS 1000

enum DispMode {
    DISP_MODE_CONSOLE = 0,
    DISP_MODE_EMU = 1,
};

static volatile DispMode dispMode = DISP_MODE_CONSOLE;
static volatile bool consoleDirty = true;
static volatile bool consoleFullRedraw = true;
static uint32_t consoleDirtyRows = 0;
static int consoleCursorX = 0;
static int consoleCursorY = 0;
static bool dispTaskStarted = false;
static volatile bool buttonMenuMode = false;
static QueueHandle_t buttonEventQueue = NULL;
static volatile bool storageMenuRequested = false;
static volatile bool displayRotate180 = false;
static volatile bool autoStatusEnabled = true;
static volatile int pendingDispMode = -1;
static portMUX_TYPE consoleMux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE modeMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool qrCodeActive = false;
static int qrCodeSize = 0;
static uint8_t qrCodeData[qrcodegen_BUFFER_LEN_MAX];
static uint8_t qrCodeTemp[qrcodegen_BUFFER_LEN_MAX];

static SemaphoreHandle_t surfaceMutex;

// Store one physical panel pixel in the Mac-layout front buffer. The LCD ISR
// performs the inverse mapping while filling each eight-line bounce buffer.
static inline void setPanelPixel(uint8_t *framebuffer, int panelX, int panelY, bool black) {
    const int macX = displayRotate180 ? (MAC_FB_W - 1) - panelY : panelY;
    const int macY = displayRotate180 ? panelX : (MAC_FB_H - 1) - panelX;
    uint8_t *packed = &framebuffer[macY * MAC_FB_STRIDE + (macX >> 3)];
    const uint8_t mask = 0x80u >> (macX & 7);
    if (black) {
        *packed |= mask;
    } else {
        *packed &= static_cast<uint8_t>(~mask);
    }
}

static void startDispTaskIfNeeded();

static void requestDispMode(DispMode mode) {
    taskENTER_CRITICAL(&modeMux);
    pendingDispMode = mode;
    taskEXIT_CRITICAL(&modeMux);
}

static void lockSurface() {
    if (surfaceMutex) {
        xSemaphoreTake(surfaceMutex, portMAX_DELAY);
    }
}

static bool tryLockSurface() {
    return surfaceMutex && xSemaphoreTake(surfaceMutex, 0) == pdTRUE;
}

static void unlockSurface() {
    if (surfaceMutex) {
        xSemaphoreGive(surfaceMutex);
    }
}

// Console Text VRAM
static char consoleVram[CONSOLE_ROWS][CONSOLE_COLS];
static uint32_t consoleInverseRows = 0;

// Clear the console text buffer and reset cursor/dirty state.
static void consoleClearLocked() {
    memset(consoleVram, ' ', CONSOLE_ROWS * CONSOLE_COLS);
    consoleInverseRows = 0;
    consoleCursorX = 0;
    consoleCursorY = 0;
    consoleDirtyRows = (CONSOLE_ROWS >= 32) ? 0xFFFFFFFFu : ((1u << CONSOLE_ROWS) - 1u);
    consoleDirty = true;
    consoleFullRedraw = true;
}

// Advance to a new console line, scrolling only the log area when needed.
static void consoleNewlineLocked() {
    consoleCursorX = 0;
    if (consoleCursorY < CONSOLE_STATUS_ROW - 1) {
        consoleCursorY++;
        return;
    }
    memmove(consoleVram[0], consoleVram[1], (CONSOLE_STATUS_ROW - 1) * CONSOLE_COLS);
    memset(consoleVram[CONSOLE_STATUS_ROW - 1], ' ', CONSOLE_COLS);
    consoleInverseRows = (consoleInverseRows >> 1) & ((1u << CONSOLE_STATUS_ROW) - 1u);
    consoleDirtyRows = (CONSOLE_ROWS >= 32) ? 0xFFFFFFFFu : ((1u << CONSOLE_ROWS) - 1u);
    consoleDirty = true;
    consoleFullRedraw = true;
}

// Append one character to the console buffer with simple terminal-like behavior.
static void consolePutCharLocked(char ch) {
    if (ch == '\r') {
        return;
    }
    if (ch == '\n') {
        consoleNewlineLocked();
        return;
    }
    if (ch == '\t') {
        int spaces = 4 - (consoleCursorX & 3);
        while (spaces--) {
            consolePutCharLocked(' ');
        }
        return;
    }
    if (ch < 32 || ch > 126) {
        ch = '?';
    }
    consoleVram[consoleCursorY][consoleCursorX++] = ch;
    if (consoleCursorX >= CONSOLE_COLS) {
        consoleNewlineLocked();
    }
    consoleDirtyRows |= (1u << consoleCursorY);
    consoleDirty = true;
}

// Test whether one scaled console glyph pixel should be lit.
static inline bool consoleGlyphPixel(char ch, int x, int y) {
    if (ch < 32 || ch > 126) {
        ch = '?';
    }
    if (x <= 0 || x >= 11 || y <= 1 || y >= 23) {
        return false;
    }
    int srcRow = (y - 2) / CONSOLE_FONT_SCALE_Y;
    if (srcRow < 0 || srcRow >= 7) {
        return false;
    }
    int srcCol = (x - 1) / CONSOLE_FONT_SCALE_X;
    if (srcCol < 0 || srcCol >= 5) {
        return false;
    }
    uint8_t row = consoleFont5x7[ch - 32][srcRow];
    return row & (0x10 >> srcCol);
}

// Erase one console row in the Mac-layout front buffer before repainting it.
static void clearConsoleRowToFramebuffer(uint8_t *framebuffer, int row, bool black) {
    const int logY0 = row * CONSOLE_CHAR_H;
    const int logY1 = logY0 + CONSOLE_CHAR_H;

    for (int logY = logY0; logY < logY1; ++logY) {
        int panelX = (CONSOLE_LOG_H - 1) - logY;
        if (displayRotate180) {
            panelX = DISP_W - 1 - panelX;
        }
        for (int logX = 0; logX < CONSOLE_LOG_W; ++logX) {
            int panelY = displayRotate180 ? (DISP_H - 1 - logX) : logX;
            setPanelPixel(framebuffer, panelX, panelY, black);
        }
    }
}

// Render one console row of text into the Mac-layout front buffer.
static void renderConsoleRowToFramebuffer(uint8_t *framebuffer, const char *rowChars, int row, bool inverse) {
    clearConsoleRowToFramebuffer(framebuffer, row, !inverse);
    for (int col = 0; col < CONSOLE_COLS; col++) {
        char ch = rowChars[col];
        for (int gy = 0; gy < CONSOLE_CHAR_H; gy++) {
            for (int gx = 0; gx < CONSOLE_CHAR_W; gx++) {
                bool set = consoleGlyphPixel(ch, (CONSOLE_CHAR_W - 1) - gx, gy);
                if (set) {
                    int logX = col * CONSOLE_CHAR_W + gx;
                    int logY = row * CONSOLE_CHAR_H + gy;
                    int panelX = (CONSOLE_LOG_H - 1) - logY;
                    int panelY = logX;
                    if (displayRotate180) {
                        panelX = DISP_W - 1 - panelX;
                        panelY = DISP_H - 1 - panelY;
                    }
                    setPanelPixel(framebuffer, panelX, panelY, inverse);
                }
            }
        }
    }
}

// Paint a captured console snapshot into the Mac-layout front buffer.
// The caller chooses between a full redraw and a dirty-row update.
static void renderConsoleSnapshotToFramebuffer(uint8_t *framebuffer, char snapshot[CONSOLE_ROWS][CONSOLE_COLS],
                                               uint32_t inverseRows, uint32_t dirtyRows, bool redrawAll) {
    if (redrawAll) {
        // Full redraw is used for mode switches and orientation changes.
        memset(framebuffer, 0xFF, FRAMEBUFFER_SIZE);
        for (int row = 0; row < CONSOLE_ROWS; ++row) {
            renderConsoleRowToFramebuffer(framebuffer, snapshot[row], row,
                                          (inverseRows & (1u << row)) != 0);
        }
        return;
    }

    while (dirtyRows) {
        // Incremental redraw keeps console updates cheap during boot/runtime logs.
        int row = __builtin_ctz(dirtyRows);
        dirtyRows &= dirtyRows - 1;
        if (row < CONSOLE_ROWS) {
            renderConsoleRowToFramebuffer(framebuffer, snapshot[row], row,
                                          (inverseRows & (1u << row)) != 0);
        }
    }
}

static void fillConsoleRect(uint8_t *framebuffer, int logX, int logY, int width, int height, bool black) {
    if (logX < 0) {
        width += logX;
        logX = 0;
    }
    if (logY < 0) {
        height += logY;
        logY = 0;
    }
    if (logX + width > CONSOLE_LOG_W) {
        width = CONSOLE_LOG_W - logX;
    }
    if (logY + height > CONSOLE_LOG_H) {
        height = CONSOLE_LOG_H - logY;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    for (int y = logY; y < logY + height; ++y) {
        int panelX = (CONSOLE_LOG_H - 1) - y;
        if (displayRotate180) {
            panelX = DISP_W - 1 - panelX;
        }
        for (int x = logX; x < logX + width; ++x) {
            int panelY = displayRotate180 ? (DISP_H - 1 - x) : x;
            setPanelPixel(framebuffer, panelX, panelY, black);
        }
    }
}

static void renderQrCodeToFramebuffer(uint8_t *framebuffer) {
    if (!qrCodeActive || qrCodeSize <= 0) {
        return;
    }

    constexpr int quietZoneModules = 4;
    constexpr int qrFirstRow = 8;
    const int totalModules = qrCodeSize + quietZoneModules * 2;
    const int availableHeight = ((CONSOLE_STATUS_ROW - qrFirstRow) * CONSOLE_CHAR_H) - 16;
    const int availableWidth = CONSOLE_LOG_W - 64;
    const int maxSide = availableWidth < availableHeight ? availableWidth : availableHeight;
    int scale = maxSide / totalModules;
    if (scale < 2) {
        scale = 2;
    }
    const int qrSide = totalModules * scale;
    const int logX = (CONSOLE_LOG_W - qrSide) / 2;
    const int logY = qrFirstRow * CONSOLE_CHAR_H;

    fillConsoleRect(framebuffer, logX, logY, qrSide, qrSide, false);
    for (int y = 0; y < qrCodeSize; ++y) {
        for (int x = 0; x < qrCodeSize; ++x) {
            if (!qrcodegen_getModule(qrCodeData, x, y)) {
                continue;
            }
            fillConsoleRect(framebuffer,
                            logX + (x + quietZoneModules) * scale,
                            logY + (y + quietZoneModules) * scale,
                            scale,
                            scale,
                            true);
        }
    }
}

// Snapshot the shared console buffer and render it without holding the lock.
static void renderConsoleToFramebuffer(uint8_t *framebuffer) {
    char snapshot[CONSOLE_ROWS][CONSOLE_COLS];
    uint32_t dirtyRows = 0;
    uint32_t inverseRows = 0;
    bool redrawAll = false;
    taskENTER_CRITICAL(&consoleMux);
    memcpy(snapshot, consoleVram, sizeof(snapshot));
    inverseRows = consoleInverseRows;
    dirtyRows = consoleDirtyRows;
    redrawAll = consoleFullRedraw;
    consoleDirtyRows = 0;
    consoleDirty = false;
    consoleFullRedraw = false;
    taskEXIT_CRITICAL(&consoleMux);

    renderConsoleSnapshotToFramebuffer(framebuffer, snapshot, inverseRows, dirtyRows, redrawAll);
    renderQrCodeToFramebuffer(framebuffer);
}

// Rasterize dirty console text into the 1bpp front buffer. Emulated Mac video
// is written by the emulator and scanned out directly by the RGB LCD peripheral.
static void consoleRenderTaskFunc(void *param) {
    printf("CONSOLE_RENDER: task running on core %d\n", xPortGetCoreID());

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONSOLE_RENDER_POLL_MS));
        if (!panel_handle) {
            vTaskDelay(10);
            continue;
        }

        if (dispMode == DISP_MODE_CONSOLE && vmuConsoleActive() &&
            (consoleDirty || consoleFullRedraw) &&
            tryLockSurface()) {
            if (dispMode == DISP_MODE_CONSOLE && vmuConsoleActive()) {
                renderConsoleToFramebuffer(vmuGetFrontBuffer());
            }
            unlockSurface();
        }
    }
}

static void cycleBacklight() {
    static const uint8_t levels[] = {100, 60, 30, 0};
    size_t next = 0;
    for (size_t i = 0; i < sizeof(levels); ++i) {
        if (LCD_Backlight == levels[i]) {
            next = (i == (sizeof(levels) - 1) ? 0 : i + 1);
            break;
        }
    }
    Set_Backlight(levels[next]);
    printf("BACKLIGHT: %u%%\n", (unsigned)levels[next]);
}

static void handleButtonShortPress() {
    if (buttonMenuMode) {
        int event = 1;
        xQueueSend(buttonEventQueue, &event, 0);
    } else {
        cycleBacklight();
    }
}

static void handleButtonLongPress() {
    if (buttonMenuMode) {
        int event = 2;
        xQueueSend(buttonEventQueue, &event, 0);
    } else if (dispMode == DISP_MODE_CONSOLE) {
        storageMenuRequested = true;
    } else {
        dispShowConsole();
    }
}

// Poll GPIO0 and dispatch debounced short- and long-press gestures.
static void buttonTaskFunc(void *param) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    int last = 1;
    TickType_t pressedAt = 0;
    bool longPressHandled = false;
    while (1) {
        int level = gpio_get_level(BOOT_BUTTON_GPIO);
        TickType_t now = xTaskGetTickCount();

        if (last == 1 && level == 0) {
            pressedAt = now;
            longPressHandled = false;
        } else if (level == 0 && !longPressHandled &&
                   now - pressedAt >= pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS)) {
            handleButtonLongPress();
            longPressHandled = true;
        } else if (last == 0 && level == 1 && !longPressHandled &&
                   now - pressedAt >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
            handleButtonShortPress();
        }
        last = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Lazily start the display refresh and mode-toggle tasks.
static void startDispTaskIfNeeded() {
    if (dispTaskStarted) {
        return;
    }
    surfaceMutex = xSemaphoreCreateMutex();
    buttonEventQueue = xQueueCreate(4, sizeof(int));
    dispTaskStarted = true;
    xTaskCreatePinnedToCore(consoleRenderTaskFunc, "console_render",
                            4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(buttonTaskFunc, "boot_btn", 2048, NULL, 2, NULL, 0);;
}

void dispSetButtonMenuMode(bool enabled) {
    buttonMenuMode = enabled;
    if (buttonEventQueue) {
        xQueueReset(buttonEventQueue);
    }
}

int dispWaitButtonEvent(uint32_t timeoutMs) {
    int event = 0;
    if (buttonEventQueue) {
        TickType_t wait = timeoutMs == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
        xQueueReceive(buttonEventQueue, &event, wait);
    }
    return event;
}

bool dispBootButtonPressed() {
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

bool dispTakeStorageMenuRequest() {
    bool requested = storageMenuRequested;
    storageMenuRequested = false;
    return requested;
}

// Initialize the display pipeline and switch to emulator mode.
void dispInit() {
    printf("DISP: bounce-only VMU front buffer, 1:1 rotated %dx%d\n",
           IMG_W, IMG_H);
    startDispTaskIfNeeded();
    dispShowEmu();
}

// Apply queued mode changes from the emulator task, between CPU slices.
void dispApplyPendingMode(void) {
    int mode = -1;
    taskENTER_CRITICAL(&modeMux);
    mode = pendingDispMode;
    pendingDispMode = -1;
    taskEXIT_CRITICAL(&modeMux);

    if (mode < 0) {
        return;
    }

    lockSurface();
    if (mode == DISP_MODE_CONSOLE) {
        if (!vmuConsoleActive()) {
            renderConsoleToFramebuffer(vmuGetMappedBuffer(VMU_SURFACE_CONSOLE));
        }
        vmuShowConsole();
    } else {
        vmuShowMac();
    }
    unlockSurface();
}

// Change LCD rotation and force the console surface to redraw.
void dispSetRotate180(bool rotate180) {
    displayRotate180 = rotate180;
    LCD_SetMacFrameBuffer(vmuGetFrontBuffer(), rotate180);
    taskENTER_CRITICAL(&consoleMux);
    consoleDirty = true;
    consoleFullRedraw = true;
    taskEXIT_CRITICAL(&consoleMux);
    printf("DISP: rotate180=%s\n", rotate180 ? "on" : "off");
}

// Initialize the onscreen boot console and make it the active display mode.
void dispConsoleInit() {
    startDispTaskIfNeeded();
    taskENTER_CRITICAL(&consoleMux);
    consoleClearLocked();
    taskEXIT_CRITICAL(&consoleMux);
    dispMode = DISP_MODE_CONSOLE;
}

// Clear the boot-console text buffer.
void dispConsoleClear() {
    taskENTER_CRITICAL(&consoleMux);
    consoleClearLocked();
    taskEXIT_CRITICAL(&consoleMux);
}

// Append formatted text to the onscreen boot console.
void dispConsolePrintf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    taskENTER_CRITICAL(&consoleMux);
    for (const char *p = buf; *p; p++) {
        consolePutCharLocked(*p);
    }
    taskEXIT_CRITICAL(&consoleMux);
}

void dispConsolePrintfInverse(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    taskENTER_CRITICAL(&consoleMux);
    consoleInverseRows |= 1u << consoleCursorY;
    for (const char *p = buf; *p; p++) {
        consolePutCharLocked(*p);
    }
    taskEXIT_CRITICAL(&consoleMux);
}

void dispConsoleWriteRow(uint8_t row, bool inverse, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    taskENTER_CRITICAL(&consoleMux);
    if (row < CONSOLE_ROWS) {
        memset(consoleVram[row], ' ', CONSOLE_COLS);
        if (inverse) {
            consoleInverseRows |= 1u << row;
        } else {
            consoleInverseRows &= ~(1u << row);
        }
        int x = 0;
        for (const char *p = buf; *p && *p != '\n' && x < CONSOLE_COLS; ++p) {
            char ch = *p;
            if (ch < 32 || ch > 126) {
                ch = '?';
            }
            consoleVram[row][x++] = ch;
        }
        consoleDirtyRows |= (1u << row);
        consoleDirty = true;
    }
    taskEXIT_CRITICAL(&consoleMux);
}

static void setConsoleStatusText(const char *buf) {
    taskENTER_CRITICAL(&consoleMux);
    memset(consoleVram[CONSOLE_STATUS_ROW], ' ', CONSOLE_COLS);
    consoleInverseRows &= ~(1u << CONSOLE_STATUS_ROW);
    int x = 0;
    for (const char *p = buf; *p && *p != '\n' && x < CONSOLE_COLS; ++p) {
        char ch = *p;
        if (ch < 32 || ch > 126) {
            ch = '?';
        }
        consoleVram[CONSOLE_STATUS_ROW][x++] = ch;
    }
    consoleDirtyRows |= (1u << CONSOLE_STATUS_ROW);
    consoleDirty = true;
    taskEXIT_CRITICAL(&consoleMux);
}

// Update the fixed console status row without scrolling the log area.
void dispConsoleSetStatus(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    setConsoleStatusText(buf);
}

void dispConsoleSetAutoStatus(const char *fmt, ...) {
    if (!autoStatusEnabled) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (autoStatusEnabled) {
        setConsoleStatusText(buf);
    }
}

bool dispConsoleShowQrCode(const char *title, const char *url, const char *hint) {
    if (!url || url[0] == '\0') {
        return false;
    }

    lockSurface();
    const bool ok = qrcodegen_encodeText(url,
                                         qrCodeTemp,
                                         qrCodeData,
                                         qrcodegen_Ecc_MEDIUM,
                                         qrcodegen_VERSION_MIN,
                                         qrcodegen_VERSION_MAX,
                                         qrcodegen_Mask_AUTO,
                                         true);
    if (ok) {
        qrCodeSize = qrcodegen_getSize(qrCodeData);
        qrCodeActive = true;
    }
    unlockSurface();
    if (!ok) {
        qrCodeSize = 0;
        qrCodeActive = false;
        return false;
    }

    dispShowConsole();
    dispConsoleClear();
    dispConsoleWriteRow(1, true, "%s", title ? title : "QR Code");
    dispConsoleWriteRow(3, false, "%s", url);
    dispConsoleWriteRow(5, false, "Open this URL on your phone");
    dispConsoleWriteRow(6, false, "or scan the QR code below.");
    dispConsoleSetStatus("%s", hint ? hint : "Short/Long: back");
    taskENTER_CRITICAL(&consoleMux);
    consoleDirty = true;
    consoleFullRedraw = true;
    taskEXIT_CRITICAL(&consoleMux);
    return true;
}

void dispConsoleHideQrCode(void) {
    lockSurface();
    qrCodeActive = false;
    qrCodeSize = 0;
    unlockSurface();
    taskENTER_CRITICAL(&consoleMux);
    consoleDirty = true;
    consoleFullRedraw = true;
    taskEXIT_CRITICAL(&consoleMux);
}

void dispSetAutoStatusEnabled(bool enabled) {
    autoStatusEnabled = enabled;
}

// Switch the display task back to the boot console surface.
void dispShowConsole() {
    taskENTER_CRITICAL(&consoleMux);
    consoleDirty = true;
    consoleFullRedraw = true;
    taskEXIT_CRITICAL(&consoleMux);
    dispMode = DISP_MODE_CONSOLE;
    requestDispMode(DISP_MODE_CONSOLE);
}

// Switch the display task back to the emulated Mac framebuffer.
void dispShowEmu() {
    storageMenuRequested = false;
    dispMode = DISP_MODE_EMU;
    requestDispMode(DISP_MODE_EMU);
}
