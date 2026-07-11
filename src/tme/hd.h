#pragma once

#include <stddef.h>

#include "ncr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build an SCSI disk device backed by SD card storage or flash fallback.
SCSIDevice *hdCreate(const char *file);
// Copy the currently open SD-card image path for one SCSI target, or empty.
void hdGetMountedPath(int target, char *destination, size_t destinationSize);
// Flush and close all SD-card-backed disk image files.
void hdShutdownAll(void);

#ifdef __cplusplus
}
#endif
