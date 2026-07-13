#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "esp_err.h"
#include "tme/snd.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audioRmtInit(void);
void audioRmtSetEnabled(bool enabled);
bool audioRmtWriteFrame(const uint8_t *rawSamples,
                        int volume,
                        bool frameStartGateEnabled,
                        const SndGateEvent *events,
                        uint16_t eventCount);

#ifdef __cplusplus
}
#endif
