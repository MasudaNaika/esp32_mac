#ifndef INPUT_HOST_H
#define INPUT_HOST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint8_t scancode;
	uint8_t isRelease;
} InputHostKeyEvent;

// Record host-side mouse movement/button state from BLE or web input.
void inputHostMouseMove(int dx, int dy, int buttonDown);
// Move accumulated host mouse input into the emulator-owned mouse device.
void inputHostDrainMouse(int *dx, int *dy, int *buttonDown);
// Clear pending motion and release the host-side mouse button.
void inputHostReleaseMouse(void);

// Record one host-side key edge.
void inputHostKey(uint8_t scancode, bool down);
// Pop the next key edge for the emulated keyboard device.
bool inputHostPopKeyEvent(InputHostKeyEvent *event);
// Release every host-held key, used on disconnect/focus-loss recovery.
void inputHostReleaseAllKeys(void);
// Clear all host input state at machine reset.
void inputHostReset(void);

#ifdef __cplusplus
}
#endif

#endif
