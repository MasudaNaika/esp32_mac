#include "emu_monitor.h"

#include <stdbool.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "app_settings.h"
#include "console_log.h"
#include "disp.h"
#include "mmu.h"

typedef struct {
	// Snapshot of recent per-core usage derived from the two idle tasks.
	int coreUsage[2];
	bool valid;
} CoreUsageSample;

typedef struct {
	// Perf sample handed from the emulation thread to the logger task.
	int64_t sampleTimeUs;
	int64_t emuBusyUs;
	bool audioSync;
	MmuStats mmuStats;
} PendingPerfSample;

static int64_t gEmuBusyUsAccum = 0;
static int64_t gEmuBusyUsSample = 0;
static bool gDetailedLogEnabled = false;
static bool gCoreUsageEnabled = true;
static bool gPerfOutputEnabled = true;
static configRUN_TIME_COUNTER_TYPE gPrevIdleRuntime[2] = {0, 0};
static int64_t gPrevCoreSampleTimeUs = 0;
static portMUX_TYPE gPerfSampleMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool gPerfSamplePending = false;
static PendingPerfSample gPendingPerfSample = {0};
static bool gPerfTaskStarted = false;
static CoreUsageSample gCachedCoreSample = {
	.coreUsage = { -1, -1 },
	.valid = false,
};

static CoreUsageSample sampleCoreUsage(void);
static void perfMonitorTask(void *param);

// Lazily start the background perf logger task.
static void ensurePerfTaskStarted(void) {
	if (gPerfTaskStarted) {
		return;
	}
	gPerfTaskStarted = true;
	xTaskCreatePinnedToCore(perfMonitorTask, "perfmon", 6144, NULL, 1, NULL, 0);
}

// Background task that prints perf samples outside the emulation loop.
// Steps:
// 1. wait for a published sample,
// 2. compute Mac speed, emulator load, and optional core usage,
// 3. print status to serial and update the onscreen status row.
static void perfMonitorTask(void *param) {
	(void)param;
	int64_t lastWallUs = 0;
	PendingPerfSample sample;

	while (1) {
		bool haveSample = false;
		taskENTER_CRITICAL(&gPerfSampleMux);
		if (gPerfSamplePending) {
			sample = gPendingPerfSample;
			gPerfSamplePending = false;
			haveSample = true;
		}
		taskEXIT_CRITICAL(&gPerfSampleMux);

		if (!haveSample) {
			vTaskDelay(pdMS_TO_TICKS(10));
			continue;
		}

		if (lastWallUs != 0) {
			const int64_t wallUs = sample.sampleTimeUs - lastWallUs;
			const int64_t idealUs = EMU_IDEAL_TIME_US;
			int macSpeed = wallUs > 0 ? (int)((idealUs * 100) / wallUs) : 0;
			int emuLoad = idealUs > 0 ? (int)((sample.emuBusyUs * 100) / idealUs) : 0;
			int emuHeadroom = 100 - emuLoad;
			CoreUsageSample coreSample = {
				.coreUsage = { -1, -1 },
				.valid = false,
			};
			if (gCoreUsageEnabled) {
				gCachedCoreSample = sampleCoreUsage();
				coreSample = gCachedCoreSample;
			}

			if (!gPerfOutputEnabled) {
				lastWallUs = sample.sampleTimeUs;
				continue;
			}

			if (appConsoleOutEnabled()) {
				char line[256];
				int used = snprintf(line, sizeof(line),
				                    "Mac %d%% Emu %d%%(%+d%%) T%c",
				                    macSpeed,
				                    emuLoad,
				                    emuHeadroom,
				                    sample.audioSync ? '-' : '@');
				if (coreSample.valid) {
					if (used > 0 && used < (int)sizeof(line)) {
						used += snprintf(line + used, sizeof(line) - used,
						                 " Core0 %d%% Core1 %d%%",
						                 coreSample.coreUsage[0],
						                 coreSample.coreUsage[1]);
					}
					if (gDetailedLogEnabled) {
						uint32_t totalReads = sample.mmuStats.read8Count + sample.mmuStats.read16Count + sample.mmuStats.read32Count;
						uint32_t totalWrites = sample.mmuStats.write8Count + sample.mmuStats.write16Count + sample.mmuStats.write32Count;
						if (used > 0 && used < (int)sizeof(line)) {
							used += snprintf(line + used, sizeof(line) - used,
							                 " PC %lu Rd %lu Wr %lu",
							                 (unsigned long)sample.mmuStats.pcChangeCount,
							                 (unsigned long)totalReads,
							                 (unsigned long)totalWrites);
						}
						if (used > 0 && used < (int)sizeof(line)) {
							used += snprintf(line + used, sizeof(line) - used,
							                 " R8/16/32 %lu/%lu/%lu",
							                 (unsigned long)sample.mmuStats.read8Count,
							                 (unsigned long)sample.mmuStats.read16Count,
							                 (unsigned long)sample.mmuStats.read32Count);
						}
						if (used > 0 && used < (int)sizeof(line)) {
							used += snprintf(line + used, sizeof(line) - used,
							                 " W8/16/32 %lu/%lu/%lu",
							                 (unsigned long)sample.mmuStats.write8Count,
							                 (unsigned long)sample.mmuStats.write16Count,
							                 (unsigned long)sample.mmuStats.write32Count);
						}
					}
				}
				consoleLogPrintf("%s\n", line);
			}
			if (coreSample.valid) {
				dispConsoleSetAutoStatus("Mac %d%% Emu %d%%(%+d%%) CPU0 %d%% CPU1 %d%% T%c",
				                     macSpeed,
				                     emuLoad,
				                     emuHeadroom,
				                     coreSample.coreUsage[0],
				                     coreSample.coreUsage[1],
				                     sample.audioSync ? '-' : '@');
			} else {
				dispConsoleSetAutoStatus("Core usage sampling...");
			}
		}

		lastWallUs = sample.sampleTimeUs;
	}
}

// Sample only the two idle task counters. Unlike uxTaskGetSystemState(), this
// avoids suspending the scheduler while every task is copied and inspected.
static CoreUsageSample sampleCoreUsage(void) {
	CoreUsageSample sample = {
		.coreUsage = { -1, -1 },
		.valid = false,
	};
	configRUN_TIME_COUNTER_TYPE idleRuntime[2] = {0, 0};
	for (BaseType_t core = 0; core < portNUM_PROCESSORS && core < 2; ++core) {
		idleRuntime[core] = ulTaskGetIdleRunTimeCounterForCore(core);
	}

	const int64_t nowUs = esp_timer_get_time();
	const int64_t elapsedUs = nowUs - gPrevCoreSampleTimeUs;
	const bool havePreviousSample = gPrevCoreSampleTimeUs != 0;
	if (!havePreviousSample || elapsedUs <= 0) {
		for (int core = 0; core < portNUM_PROCESSORS && core < 2; ++core) {
			gPrevIdleRuntime[core] = idleRuntime[core];
		}
		gPrevCoreSampleTimeUs = nowUs;
		return sample;
	}

	for (int core = 0; core < portNUM_PROCESSORS && core < 2; ++core) {
		configRUN_TIME_COUNTER_TYPE idleDelta = idleRuntime[core] - gPrevIdleRuntime[core];
		int usage = 100 - (int)((idleDelta * 100ULL) / elapsedUs);
		if (usage < 0) {
			usage = 0;
		}
		if (usage > 100) {
			usage = 100;
		}
		sample.coreUsage[core] = usage;
	}
	for (int core = 0; core < portNUM_PROCESSORS && core < 2; ++core) {
		gPrevIdleRuntime[core] = idleRuntime[core];
	}
	gPrevCoreSampleTimeUs = nowUs;

	sample.valid = true;
	return sample;
}

// Accumulate emulator busy time into the current unpublished sample window.
void emuMonitorAddBusyTime(int64_t busyUs) {
	gEmuBusyUsSample += busyUs;
}

// Report whether detailed perf logging is currently enabled.
bool emuMonitorDetailedEnabled(void) {
	return gDetailedLogEnabled;
}

// Toggle detailed perf logging.
void emuMonitorSetDetailedEnabled(bool enabled) {
	gDetailedLogEnabled = enabled;
}

// Report whether per-core usage sampling is currently enabled.
bool emuMonitorCoreUsageEnabled(void) {
	return gCoreUsageEnabled;
}

// Toggle per-core usage sampling and clear the cached sample state.
void emuMonitorSetCoreUsageEnabled(bool enabled) {
	gCoreUsageEnabled = enabled;
	gPrevCoreSampleTimeUs = 0;
	gPrevIdleRuntime[0] = 0;
	gPrevIdleRuntime[1] = 0;
	gCachedCoreSample = (CoreUsageSample){
		.coreUsage = { -1, -1 },
		.valid = false,
	};
}

// Publish one perf sample from the emulation loop to the logger task.
// Steps:
// 1. ensure the logger task exists,
// 2. snapshot busy time and MMU counters,
// 3. hand the sample to the background task through the shared slot.
void emuMonitorPublishSample(bool audioSync) {
	ensurePerfTaskStarted();
	gEmuBusyUsAccum = gEmuBusyUsSample;
	gEmuBusyUsSample = 0;

	taskENTER_CRITICAL(&gPerfSampleMux);
	gPendingPerfSample.sampleTimeUs = esp_timer_get_time();
	gPendingPerfSample.emuBusyUs = gEmuBusyUsAccum;
	gPendingPerfSample.audioSync = audioSync;
	gPendingPerfSample.mmuStats = mmuSnapshotStats();
	gPerfSamplePending = true;
	taskEXIT_CRITICAL(&gPerfSampleMux);

	gEmuBusyUsAccum = 0;
}
