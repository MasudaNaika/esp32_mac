#ifndef EMU_MONITOR_H
#define EMU_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of video frames combined into one published perf sample.
#define EMU_MONITOR_LOG_INTERVAL_FRAMES 300
#define EMU_IDEAL_TIME_US 4987745  // 130240 / 7833600 * 1000000 * 300

// Add one chunk of emulation busy time to the current sample window.
void emuMonitorAddBusyTime(int64_t busyUs);
// Check whether detailed perf output is enabled.
bool emuMonitorDetailedEnabled(void);
// Enable or disable detailed perf output.
void emuMonitorSetDetailedEnabled(bool enabled);
// Check whether per-core usage sampling is enabled.
bool emuMonitorCoreUsageEnabled(void);
// Enable or disable per-core usage sampling.
void emuMonitorSetCoreUsageEnabled(bool enabled);
// Publish the current sample window to the background logger task.
void emuMonitorPublishSample(bool audioSync);

#ifdef __cplusplus
}
#endif

#endif
