#ifndef TMECONFIG_H
#define TMECONFIG_H

// ESP32-S3 with OPI PSRAM: direct memory-mapped, no cache needed
#define TME_ROMSIZE (128*1024)
#define TME_CACHESIZE 0
#define TME_RAMSIZE (4*1024*1024)
#define TME_SCREENBUF 0x3FA700
#define TME_SCREENBUF_ALT 0x3F2700
#define TME_SNDBUF 0x3FFD00
#define TME_SNDBUF_ALT (TME_SNDBUF-0x5C00)

#endif
