#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "tme/snd.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audioRmtInit(void);
bool audioRmtWriteFrame(const uint8_t *rawSamples,
                        int volume,
                        bool frameStartGateEnabled,
                        const SndGateEvent *events,
                        uint16_t eventCount);

#ifdef __cplusplus
}
#endif
