#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t consoleLogWriteSeq(void);
uint32_t consoleLogOldestSeq(void);
size_t consoleLogRead(uint32_t *cursor, char *dst, size_t dstSize);
int consoleLogPrintf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
