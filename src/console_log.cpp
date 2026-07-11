#include "console_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "tme/tmeconfig.h"

namespace {

constexpr size_t kLogBufferSize = 8192;
char *sLogBuffer;
uint32_t sWriteSeq;
volatile bool sAllocating;
portMUX_TYPE sLogMux = portMUX_INITIALIZER_UNLOCKED;

bool ensureLogBuffer() {
    if (sLogBuffer) {
        return true;
    }
    if (sAllocating) {
        return false;
    }
    sAllocating = true;
    char *buffer = static_cast<char *>(tme_psram_aligned_alloc(kLogBufferSize,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    taskENTER_CRITICAL(&sLogMux);
    if (!sLogBuffer && buffer) {
        sLogBuffer = buffer;
    }
    bool ok = sLogBuffer != nullptr;
    taskEXIT_CRITICAL(&sLogMux);
    if (buffer && buffer != sLogBuffer) {
        heap_caps_free(buffer);
    }
    sAllocating = false;
    return ok;
}

void appendLogBytes(const char *text, size_t len) {
    if (!text || len == 0) {
        return;
    }
    if (!ensureLogBuffer()) {
        return;
    }
    taskENTER_CRITICAL(&sLogMux);
    for (size_t i = 0; i < len; ++i) {
        sLogBuffer[sWriteSeq % kLogBufferSize] = text[i];
        ++sWriteSeq;
    }
    taskEXIT_CRITICAL(&sLogMux);
}

}  // namespace

uint32_t consoleLogWriteSeq(void) {
    taskENTER_CRITICAL(&sLogMux);
    uint32_t seq = sWriteSeq;
    taskEXIT_CRITICAL(&sLogMux);
    return seq;
}

uint32_t consoleLogOldestSeq(void) {
    taskENTER_CRITICAL(&sLogMux);
    uint32_t seq = sWriteSeq > kLogBufferSize ? sWriteSeq - kLogBufferSize : 0;
    taskEXIT_CRITICAL(&sLogMux);
    return seq;
}

size_t consoleLogRead(uint32_t *cursor, char *dst, size_t dstSize) {
    if (!cursor || !dst || dstSize == 0) {
        return 0;
    }
    if (!ensureLogBuffer()) {
        return 0;
    }

    taskENTER_CRITICAL(&sLogMux);
    uint32_t oldest = sWriteSeq > kLogBufferSize ? sWriteSeq - kLogBufferSize : 0;
    if (*cursor < oldest) {
        *cursor = oldest;
    }
    uint32_t available = sWriteSeq - *cursor;
    if (available > dstSize) {
        available = dstSize;
    }
    for (uint32_t i = 0; i < available; ++i) {
        dst[i] = sLogBuffer[(*cursor + i) % kLogBufferSize];
    }
    *cursor += available;
    taskEXIT_CRITICAL(&sLogMux);
    return available;
}

int consoleLogPrintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list outAp;
    va_copy(outAp, ap);
    int written = vprintf(fmt, outAp);
    va_end(outAp);

    char line[512];
    int captured = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (captured > 0) {
        size_t len = captured < (int)sizeof(line) ? (size_t)captured : sizeof(line) - 1;
        appendLogBytes(line, len);
    }
    return written;
}
