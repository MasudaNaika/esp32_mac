/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 * Adapted for ESP32-S3 with OPI PSRAM (direct memory-mapped, no cache)
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "emu.h"
#include <string.h>
#include "tmeconfig.h"
#include "m68k.h"
#include "disp.h"
#include "iwm.h"
#include "via.h"
#include "scc.h"
#include "rtc.h"
#include "ncr.h"
#include "hd.h"
#include "emu_monitor.h"
#include "mmu.h"
#include "snd.h"
#include "mouse.h"
#include "vmu.h"
#include <stdbool.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_settings.h"
#include "pv_fdd.h"
#include "hd.h"

static void logEmuHeap(const char *tag) {
	printf("EMU heap %s: DRAM free=%u largest=%u DMA free=%u largest=%u PSRAM free=%u largest=%u\n",
	       tag,
	       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
	       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
	       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
	       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
	       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
	       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/**
https://retrocomputing.stackexchange.com/questions/13077

Tommy wrote:

I wrote an emulator earlier the year, so I can answer on the Macintosh.

The processor's clock rate is 7,833,600 Hz; the video subsystem is completely synchronous 
and completes each line in 352 processor cycles, outputting a total of 370 lines per frame.

Therefore each frame is 130,240 cycles long.

So the original Macintosh produces a touch less than 60.15 frames per second. So it's as much 
60Hz as almost any other machine you care to mention, though nowhere near NTSC timings otherwise.

(Additionally, for the curious: two pixels are output per processor clock, 512 are pixels, 
and the equivalent of an additional 192 are spent on blanking and retrace. Of the 370 line period 
that completes a frame, 342 contain pixels and the other 28 are blanking and retrace. 
So the actual output resolution is 512x342).
 */

#define EMU_CPU_HZ		7833600
#define EMU_VBI_CYCLES	130240
#define EMU_VIA_ECLOCK_DIV 10
#define EMU_VBL_RATE_HZ 60
#define EMU_MAC_SCANLINE_CYCLES 352
#define EMU_MAC_SCANLINES 370
#define EMU_SOUND_FRAME_SAMPLES EMU_MAC_SCANLINES
#define EMU_HOT_ATTR IRAM_ATTR

_Static_assert(EMU_MAC_SCANLINE_CYCLES * EMU_MAC_SCANLINES == EMU_VBI_CYCLES,
               "Mac scanline timing must cover one VBL exactly");

uint8_t *macRom;
uint8_t *macRam;
uint8_t *macSnd[2];

int rom_remap=0, video_remap=0, audio_remap=0;
int audio_volume=0, audio_en=0;
static bool emuViaIrqPending;
static bool emuSccIrqPending;
static bool emuCpuReady;
static volatile bool emuStopRequested;
static volatile bool emuStopped = true;
static uint8_t emuSoundRawFrame[EMU_SOUND_FRAME_SAMPLES];
static SndGateEvent emuSoundGateEvents[SND_GATE_MAX_EVENTS];
static uint16_t emuSoundGateEventCount;
static bool emuSoundFrameStartGateEnabled;
static uint32_t emuSoundGateOverflowFrames;
static uint32_t emuSoundGateOverflowFramesLogged;
static uint16_t scanline_start;
static uint16_t scanline_end;
static int surpassCycles = 0;

static void updateCpuIrq(void) {
	if (emuCpuReady) {
		m68k_set_irq(emuSccIrqPending ? 2 : emuViaIrqPending ? 1 : 0);
	}
}

void m68k_instruction() {
}

uint8_t unhandledAccessCb(unsigned int address, int data, int isWrite) {
    // Flag any memory access that escapes the configured map.
    unsigned int pc=m68k_get_reg(NULL, M68K_REG_PC);
	printf("Unhandled %s @ 0x%X! PC=0x%X\n", isWrite?"write":"read", address, pc);
	return 0xff;
}

uint8_t ncrAccessCb(unsigned int address, int data, int isWrite) {
    // Forward NCR SCSI accesses into the disk controller model.
    if (isWrite) {
		ncrWrite((address>>4)&0x7, (address>>9)&1, data);
		return 0;
	} else {
		return ncrRead((address>>4)&0x7, (address>>9)&1);
	}
}

uint8_t sscAccessCb(unsigned int address, int data, int isWrite) {
    // SCC is the serial-port model used by the ROM for communications.
    if (isWrite) {
		sccWrite(address, data);
		return 0;
	} else {
		return sccRead(address);
	}
}

uint8_t iwmAccessCb(unsigned int address, int data, int isWrite) {
    // The IWM shim handles floppy controller register access.
    if (isWrite) {
		iwmWrite((address>>9)&0xf, data);
		return 0;
	} else {
		return iwmRead((address>>9)&0xf);;
	}
}

uint8_t phaseReadCb(unsigned int address, int data, int isWrite) {
    // The Mac ROM samples this window during boot to check hardware timing
    // phase. Emulation has no unstable phase state, so reads report in-phase.
	(void)address; (void)data; (void)isWrite;
	return 0;
}

uint8_t scsiDmaCb(unsigned int address, int data, int isWrite) {
    // SCSI pseudo-DMA area - return 0 for now
	(void)address; (void)data; (void)isWrite;
	return 0;
}

uint8_t viaAccessCb(unsigned int address, int data, int isWrite) {
    // The VIA window is the entry point for timers, keyboard, and control lines.
	// The VIA is connected to the 68000 upper byte lane at even addresses.
	if (address & 1) {
		return isWrite ? 0 : 0xFF;
	}
    if (isWrite) {
		viaWrite((address>>9)&0xf, data);
		return 0;
	} else {
		return viaRead((address>>9)&0xf);
	}
}

static void resetMacHardware(void) {
	// Put the VIA, SCC, IWM, mouse, and memory mapping back into the
	// power-on state expected by the ROM bootstrap.
	viaInit();
	emuViaIrqPending = false;
	emuSccIrqPending = false;
	updateCpuIrq();
	viaClear(VIA_PORTA, 0x7F);
	viaSet(VIA_PORTA, 0x80);
	viaClear(VIA_PORTA, 0xFF);
	viaSet(VIA_PORTB, (1<<3));
	sccInit();
	iwmInit();
	ncrInit();
	mouseReset();
	rom_remap = 1;
	video_remap = 0;
	audio_remap = 0;
	audio_volume = 0;
	audio_en = 0;
	surpassCycles = 0;
	mmuSetRomRemap(rom_remap);
	vmuSelectMacSurface(video_remap ? VMU_SURFACE_MAIN : VMU_SURFACE_ALTERNATE);
}

static void macResetInstruction(void) {
    // The ROM can execute RESET; mirror that by restoring hardware defaults.
    printf("M68K RESET instruction: resetting Mac hardware\n");
	resetMacHardware();
}

static void ramInit() {
    // Allocate the RAM, video buffers, and sound backings before emulation starts.
    // Allocate the Mac RAM and the two framebuffer backings up front.
	logEmuHeap("before ramInit");
	printf("Using PSRAM as Mac RAM (direct memory-mapped)\n");
	macRam=(uint8_t*)tme_psram_aligned_alloc(TME_RAMSIZE, MALLOC_CAP_SPIRAM);
	assert(macRam);
	memset(macRam, 0xFF, TME_RAMSIZE);
	printf("Mac RAM allocated at %p (%d bytes)\n", macRam, TME_RAMSIZE);

	macSnd[0]=&macRam[TME_SNDBUF];
	macSnd[1]=&macRam[TME_SNDBUF_ALT];
	memset(emuSoundRawFrame, 128, sizeof(emuSoundRawFrame));
	logEmuHeap("after ramInit");
}

static void stepPeripheralClocks(int cpuCycles) {

	static int emuPendingEclockCycles = 0;

	// Keep the VIA and SCC running in lockstep with CPU execution.
	emuPendingEclockCycles += cpuCycles;
	int eclockCycles = emuPendingEclockCycles / EMU_VIA_ECLOCK_DIV;
	emuPendingEclockCycles -= eclockCycles * EMU_VIA_ECLOCK_DIV;

	if (eclockCycles > 0) {
		viaStep(eclockCycles);
		// sccTick(eclockCycles);
	}
}

void tmeStartEmu(void *rom, const char *hdImagePaths[SCSI_TARGET_COUNT],
                 const char *fddImagePaths[PV_FDD_DRIVE_COUNT]) {
	// Build the machine, then keep feeding CPU time, VBL, display, and audio forever.
	emuStopRequested = false;
	emuStopped = false;
	logEmuHeap("tmeStartEmu entry");
	macRom=(unsigned char*)rom;
	ramInit();
	logEmuHeap("after ramInit call");
	MmuContext mmuCtx = {
		.rom = macRom,
		.ram = macRam,
		.fb = {
			vmuGetMappedBuffer(VMU_SURFACE_MAIN),
			vmuGetMappedBuffer(VMU_SURFACE_ALTERNATE),
		},
		.romRemap = &rom_remap,
	};
	mmuSetContext(&mmuCtx);
	vmuAttachMmu();
	logEmuHeap("after mmu/vmu attach");
	pvFddInit(fddImagePaths, macRam, TME_RAMSIZE);
	logEmuHeap("after pvFddInit");
	resetMacHardware();
	logEmuHeap("after resetMacHardware");
	for (int id = 0; id < SCSI_TARGET_COUNT; ++id) {
		if (hdImagePaths[id] && hdImagePaths[id][0]) {
			ncrRegisterDevice(id, hdCreate(hdImagePaths[id]));
			logEmuHeap("after hdCreate");
		}
	}
	printf("Initializing m68k...\n");
	logEmuHeap("before m68k_init");
	m68k_pc_changed_handler_function(0x0);
	m68k_init();
	logEmuHeap("after m68k_init");
	printf("Setting CPU type and resetting...");
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	m68k_set_reset_instr_callback(macResetInstruction);
	m68k_pulse_reset();
	emuCpuReady = true;
	updateCpuIrq();
	printf("Done! Running.\n");
	logEmuHeap("before dispInit");
	dispInit();
	logEmuHeap("after dispInit");

	/**
	 * Aligning CPU Slices with ROM Sound API
	 *
	 * Background:
	 * The MacPlus ROM sound paths utilize specific write-start offsets:
	 * - 4-tone normal: Offset 0x172 (~sample 185)
	 * - 4-tone solo:   Offset 0x8C  (~sample 70)
	 * - Free-form:     Offset 0x40  (~sample 32)
	 *
	 * Previously, the sound was distorted because CPU slices were triggered at fixed
	 * 5-scanline intervals (370 bytes/VBI), ignoring these specific buffer entry points.
	 *
	 * New Approach:
	 * Instead of fixed-interval slices, we dynamically adjust the CPU slice timing
	 * to synchronize with the ROM sound API's write-start scanlines (32, 70, 185).
	 *
	 * Implementation:
	 * - Replace fixed 5-scanline stepping with conditional logic to trigger slices
	 *   at the critical sync points.
	 * - Current sequence: 0, 5, ..., 25, 32, 40, ..., 65, 70, ..., 180, 185, ...
	 * - Each CPU slice now calculates and updates only the segment of the sound
	 *   buffer corresponding to the scanline range since the last slice.
	 * - This eliminates the need for constant look-up tables by using branching
	 *   logic to determine the next target scanline.
	 */
	
	int ca2 = 0;
	int frame = 0, speedFrame = 0, slice = 0;

	while(!emuStopRequested) {
		// Image files are opened only on the emulator task, between guest slices.
		pvFddProcessRequests();

		int64_t frameStartUs = esp_timer_get_time();

		// The 60.15Hz interrupt(VBL) is a signal generated by a PAL oncee very 16.63ms,
		// at the start of the vertical blanking intervalf or the built-in video monitor.
		// VBL is a pulse, not a square wave. Drive both edges so the VIA sees
		// its configured active edge once per frame rather than every other frame.
		viaControlWrite(VIA_CA1, 1);
		viaControlWrite(VIA_CA1, 0);
		emuSoundFrameStartGateEnabled = audio_en;
		emuSoundGateEventCount = 0;

		for (scanline_start = 0, slice = 0; scanline_start < EMU_MAC_SCANLINES;
			 scanline_start = scanline_end, ++slice) {
			if (scanline_start == 25) {
				scanline_end = 32;
			} else if (scanline_start == 32) {
				scanline_end = 40;
			} else {
				scanline_end = scanline_start + 5;
			}

			// Compensate for the previous CPU overrun to keep the average
			// execution time aligned with the scanline clock.
			int request = EMU_MAC_SCANLINE_CYCLES * (scanline_end - scanline_start) - surpassCycles;
			int cpuCycles = m68k_execute(request);
			surpassCycles = cpuCycles - request;
			stepPeripheralClocks(cpuCycles);

			// Capture raw Sound Manager output. Gate timing is passed separately
			// so RMT/SPI-style backends can merge the saved PB7 transitions
			// with the same post-execute slice data used by the older PWM path.
			uint8_t *source = macSnd[audio_remap ? 0 : 1];
			for (uint16_t i = scanline_start; i < scanline_end; ++i) {
				emuSoundRawFrame[i] = source[i * 2];
			}

			if ((slice & 1) == 1) {
				mouseTick();
			}

		}

		// Apply display surface switches at the VBI boundary; Mac screen scanout
		// itself is continuous, while the console renderer refreshes on its task.
		dispApplyPendingMode();

		// VBL and 1-second events arrive as VIA input control-line edges on CA1/CA2.
		++frame;
		if (frame == (EMU_VBL_RATE_HZ - 1)) {
			ca2 ^= 1;
			viaControlWrite(VIA_CA2, ca2);
		} else if (frame >= EMU_VBL_RATE_HZ) {
			ca2 ^= 1;
			viaControlWrite(VIA_CA2, ca2);
			frame = 0;
			rtcTick();
		}

		// Feed one raw sound frame plus PB7 gate transitions every VBL.
		bool hasSound = sndPushMacFrame(emuSoundRawFrame,
		                                audio_volume,
		                                emuSoundFrameStartGateEnabled,
		                                emuSoundGateEvents,
		                                emuSoundGateEventCount);
		if (emuSoundGateOverflowFrames != emuSoundGateOverflowFramesLogged) {
			emuSoundGateOverflowFramesLogged = emuSoundGateOverflowFrames;
			printf("AUDIO: gate event overflow frames=%u\n", (unsigned)emuSoundGateOverflowFramesLogged);
		}

		// Turbo mode has no audio wait and can otherwise starve IDLE1 long enough
		// to trigger the task watchdog. Yield one tick per guest second only while
		// realtime audio synchronization is being skipped.
		if (!hasSound && (speedFrame % EMU_VBL_RATE_HZ) == (EMU_VBL_RATE_HZ - 1)) {
			vTaskDelay(1);
		}

		emuMonitorAddBusyTime(esp_timer_get_time() - frameStartUs);
		if (++speedFrame>=EMU_MONITOR_LOG_INTERVAL_FRAMES) {
			speedFrame=0;
			appSetRealtimeSyncSkipped(!hasSound);
			emuMonitorPublishSample(hasSound);
		}

	}
	emuCpuReady = false;
	pvFddShutdown();
	hdShutdownAll();
	emuStopped = true;
	printf("Emulator stopped\n");
}

void tmeRequestStop(void) {
	emuStopRequested = true;
}

bool tmeEmuStopped(void) {
	return emuStopped;
}

void viaIrq(int req) {
	emuViaIrqPending = req != 0;
	updateCpuIrq();
}

void sccIrq(int req) {
	emuSccIrqPending = req != 0;
	updateCpuIrq();
}


void viaCbPortAWrite(unsigned int val) {
	// Port A carries the bank-switch and floppy-head signals from the ROM.
	// VIA port A controls ROM/video/audio bank switches and floppy head select.
	video_remap=(val&(1<<6))?1:0;
	vmuSelectMacSurface(video_remap ? VMU_SURFACE_MAIN : VMU_SURFACE_ALTERNATE);
	rom_remap=(val&(1<<4))?1:0;
	audio_remap=(val&(1<<3))?1:0;
	audio_volume=(val&7);
	iwmSetHeadSel(val&(1<<5));
	mmuSetRomRemap(rom_remap);
}

void viaCbPortBWrite(unsigned int val) {

	bool prev_audio_en = audio_en;

    // Port B carries RTC serial signaling and the active-low /SNDRES line.
	int b;
	b=rtcCom(val&4, val&1, val&2);
	if (b) viaSet(VIA_PORTB, 1); else viaClear(VIA_PORTB, 1);
	audio_en=!(val&(1<<7));

	// Record only gate edges.  PWM/PDM consume this list while rendering the
	// frame, so keeping the state change at its original CPU-cycle position is
	// more accurate than sampling audio_en once per scanline.
	if (prev_audio_en != audio_en) {
		// m68k_cycles_run() is relative to the current CPU slice.  Add the
		// previous slice's overrun correction so the event remains on the
		// Macintosh's continuous 352-cycle scanline timeline.
		int sliceCycle = m68k_cycles_run() + surpassCycles;
		if (emuSoundGateEventCount < SND_GATE_MAX_EVENTS) {
			SndGateEvent *event = &emuSoundGateEvents[emuSoundGateEventCount++];
			// Convert the absolute cycle within this frame to line/cycle
			// coordinates used by the audio backends.
			event->line = scanline_start + sliceCycle / EMU_MAC_SCANLINE_CYCLES;
			event->cycle = sliceCycle % EMU_MAC_SCANLINE_CYCLES;
			event->state = audio_en ? SND_GATE_ENABLE : SND_GATE_DISABLE;
		} else {
			// Do not overwrite earlier edges; report that this frame could
			// not represent every transition.
			++emuSoundGateOverflowFrames;
		}
	}
}
