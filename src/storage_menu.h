#pragma once

#include <stddef.h>

#include "pv_fdd.h"
#include "tme/ncr.h"

enum class StorageMenuResult { ReturnToEmu, ReturnToConsole, RestartEmu, UsbStorage };

// Let BOOT-button users select images from /sd/fd and /sd/hd at startup.
StorageMenuResult storageMenuRun(char fddNames[PV_FDD_DRIVE_COUNT][128],
                                 char hdNames[SCSI_TARGET_COUNT][128],
                                 const char *const mountedFdds[PV_FDD_DRIVE_COUNT] = nullptr,
                                 const char *const mountedHds[SCSI_TARGET_COUNT] = nullptr);

// Start the task that handles a console-view long press during emulation.
void storageMenuStartRuntime(const char mountedFdds[PV_FDD_DRIVE_COUNT][128],
                             const char mountedHds[SCSI_TARGET_COUNT][128]);
