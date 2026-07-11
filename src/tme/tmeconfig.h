#ifndef TMECONFIG_H
#define TMECONFIG_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"

// ESP32-S3 with OPI PSRAM: direct memory-mapped, no cache needed
#define TME_ROMSIZE (128*1024)
#define TME_CACHESIZE 0
#define TME_RAMSIZE (4*1024*1024)
#define TME_PSRAM_CACHE_ALIGNMENT 32
// Video memory mapped outside main RAM (like Mini vMac's large screen hack)
#define TME_VIDMEM_SIZE 0x009600        // 0x009600 bytes = 640x480 / 8 (1 bit per pixel)
#define TME_VIDMEM_BASE 0x00540000      // 68K address of video memory region
#define TME_VIDMEM_SIZE_ALT 0x009600    // 
#define TME_VIDMEM_BASE_ALT (TME_VIDMEM_BASE-TME_VIDMEM_SIZE_ALT)

// sound buffers
#define TME_SNDBUF 0x3FFD00
#define TME_SNDBUF_ALT (TME_SNDBUF-0x5C00)

static inline void *tme_psram_aligned_alloc(size_t size, uint32_t caps) {
    return heap_caps_aligned_alloc(TME_PSRAM_CACHE_ALIGNMENT, size, caps);
}

static inline void *tme_psram_aligned_calloc(size_t count, size_t size, uint32_t caps) {
    if (size != 0 && count > ((size_t)-1) / size) {
        return NULL;
    }
    size_t total = count * size;
    void *ptr = tme_psram_aligned_alloc(total, caps);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}
#endif
