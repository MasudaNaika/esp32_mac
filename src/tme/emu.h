
#include "pv_fdd.h"
#include "ncr.h"

// Start the emulator loop with the already-loaded ROM and storage paths.
void tmeStartEmu(void *rom, const char *hdImagePaths[SCSI_TARGET_COUNT],
                 const char *fddImagePaths[PV_FDD_DRIVE_COUNT]);
// Request the emulator loop to stop at the next safe slice boundary.
void tmeRequestStop(void);
// Report whether the emulator task has exited its main loop.
bool tmeEmuStopped(void);
// Feed mouse movement/button state into the emulated hardware model.
void tmeMouseMovement(int dx, int dy, int btn);
