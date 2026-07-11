#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PV_FDD_GUEST_ADDR 0x00520000u
#define PV_FDD_DRIVE_COUNT 2

// Install the guest RAM backing and optionally open each initial raw image.
// Both PV drives remain enabled when their configured names are empty.
bool pvFddInit(const char *configuredNames[PV_FDD_DRIVE_COUNT],
               uint8_t *guestRam, size_t guestRamSize);
// Close all images and clear all guest drive state.
void pvFddShutdown(void);
// Handle one byte written by the guest driver to the PV transport address.
void pvFddHandleWrite(uint8_t operation);
// Queue an image selection for the emulator task. A disk already mounted in the
// guest must be ejected there before another image can be inserted.
bool pvFddRequestImage(uint8_t drive, const char *configuredName);
// Apply queued image changes from the emulator task.
void pvFddProcessRequests(void);
// Copy the currently open configured image name, or an empty string.
void pvFddGetMountedName(uint8_t drive, char *destination, size_t destinationSize);
// Report whether the PV floppy transport is active.
bool pvFddConfigured(void);

#ifdef __cplusplus
}
#endif
