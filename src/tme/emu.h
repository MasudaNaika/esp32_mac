
#include "pv_fdd.h"
#include "ncr.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the emulator loop with the already-loaded ROM and storage paths.
void tmeStartEmu(void *rom, const char *hdImagePaths[SCSI_TARGET_COUNT],
                 const char *fddImagePaths[PV_FDD_DRIVE_COUNT]);
// Request the emulator loop to stop at the next safe slice boundary.
void tmeRequestStop(void);
// Resume the emulator after a stop request.
void tmeRequestRun(void);
// Report whether the emulator task has exited its main loop.
bool tmeEmuStopped(void);
// Report whether the guest is currently executing.
bool tmeEmuRunning(void);
// Feed mouse movement/button state into the emulated hardware model.
void tmeMouseMovement(int dx, int dy, int btn);

#ifdef __cplusplus
}
#endif
