/*
 * Sound stub for Mac Plus emulator — no audio output on this board.
 */
#include <stdint.h>
#include "esp_timer.h"

extern "C" {
#include "tme/snd.h"
}

static int64_t frameStartUs;

void sndInit() {
    frameStartUs = esp_timer_get_time();
}

// Returns 1 when ~1/60th of a second has elapsed (real-time pacing)
int sndDone() {
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - frameStartUs;
    return (elapsed >= 16667); // ~60fps
}

int sndPush(uint8_t *data, int volume) {
    (void)data;
    (void)volume;
    frameStartUs = esp_timer_get_time();
    return 0;
}
