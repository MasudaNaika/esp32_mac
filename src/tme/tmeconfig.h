#ifndef TMECONFIG_H
#define TMECONFIG_H

// ESP32-S3 with OPI PSRAM: direct memory-mapped, no cache needed
#define TME_ROMSIZE (128*1024)
#define TME_CACHESIZE 0
#define TME_RAMSIZE (4*1024*1024)
// Video memory mapped outside main RAM (like Mini vMac's large screen hack)
#define TME_VIDMEM_BASE 0x500000   // 68K address of video memory region
#define TME_VIDMEM_SIZE 0x80000    // 512KB video memory region
#define TME_SCREENBUF   0x40000    // offset within vidMem → 68K addr 0x540000
#define TME_SCREENBUF_ALT 0x30000  // offset within vidMem → 68K addr 0x530000
#define TME_SNDBUF 0x3FFD00
#define TME_SNDBUF_ALT (TME_SNDBUF-0x5C00)

#endif
