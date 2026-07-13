#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SND_GATE_MAX_EVENTS 128

typedef enum {
    SND_GATE_DISABLE = 0,
    SND_GATE_ENABLE = 1,
} SndGateState;

typedef struct {
    uint16_t line;
    uint16_t cycle;
    uint8_t state;
} SndGateEvent;

// Initialize the selected audio backend and local queue bookkeeping.
void sndInit();
// Enable or mute the physical audio output without changing guest state.
void sndSetEnabled(bool enabled);

// Submit one raw 370-byte Mac sound frame plus the /SNDRES gate transitions.
bool sndPushMacFrame(const uint8_t *rawSamples,
                     int volume,
                     bool frameStartGateEnabled,
                     const SndGateEvent *events,
                     uint16_t eventCount);

#ifdef __cplusplus
}
#endif
