/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include "scc.h"

/*
 * Minimal Zilog 8530 SCC model.
 *
 * The ESP32-S3 build has no physical serial/LocalTalk interface. Keep only the
 * status and register behavior needed by the Mac ROM and by mouse DCD updates;
 * drop the old LocalTalk RX/TX FIFOs, which consumed about 80 KiB of DRAM.
 */

void sccIrq(int ena);

#define SCC_WR15_BREAK  (1 << 7)
#define SCC_WR15_TXU    (1 << 6)
#define SCC_WR15_CTS    (1 << 5)
#define SCC_WR15_SYNC   (1 << 4)
#define SCC_WR15_DCD    (1 << 3)
#define SCC_WR15_ZCOUNT (1 << 1)

#define SCC_RR3_CHB_EXT (1 << 0)
#define SCC_RR3_CHB_TX  (1 << 1)
#define SCC_RR3_CHB_RX  (1 << 2)
#define SCC_RR3_CHA_EXT (1 << 3)
#define SCC_RR3_CHA_TX  (1 << 4)
#define SCC_RR3_CHA_RX  (1 << 5)

#define SCC_R0_RX        (1 << 0)
#define SCC_R0_ZEROCOUNT (1 << 1)
#define SCC_R0_TXE       (1 << 2)
#define SCC_R0_DCD       (1 << 3)
#define SCC_R0_SYNCHUNT  (1 << 4)
#define SCC_R0_CTS       (1 << 5)
#define SCC_R0_EOM       (1 << 6)
#define SCC_R0_BREAKABRT (1 << 7)

// Per-channel SCC state.
// Each channel keeps only the modem/control bits and interrupt latches that the
// Mac ROM still observes in this trimmed-down build.
typedef struct {
	int dcd;
	int cts;
	int hunting;
	int rr0Prev;
	int rr0Latched;
	uint8_t wr1;
	uint8_t wr15;
} SccChan;

// Whole-chip SCC state for both channels plus shared interrupt bookkeeping.
typedef struct {
	uint8_t regptr;
	uint8_t wr2;
	uint8_t wr9;
	uint8_t intpending;
	uint8_t irqAsserted;
	SccChan chan[2];
} Scc;

static Scc scc;

// Decode which SCC channel the Mac address is targeting.
static int chan_from_addr(unsigned int addr) {
	return (addr & (1 << 1)) ? SCC_CHANA : SCC_CHANB;
}

// Map one channel number to its external-interrupt bit in RR3.
static uint8_t rr3_mask_for_ext(int chan) {
	return (chan == SCC_CHANA) ? SCC_RR3_CHA_EXT : SCC_RR3_CHB_EXT;
}

// Synthesize RR0 from the current channel line state.
static uint8_t calcRr0(int chan) {
	uint8_t val = SCC_R0_TXE | SCC_R0_CTS | SCC_R0_EOM;
	if (scc.chan[chan].dcd) {
		val |= SCC_R0_DCD;
	}
	if (scc.chan[chan].hunting) {
		val |= SCC_R0_SYNCHUNT;
	}
	if (scc.chan[chan].cts) {
		val |= SCC_R0_CTS;
	}
	return val;
}

// Recompute the shared SCC IRQ output after any pending/enable change.
static void assessIrq(void) {
	int active = 0;
	if ((scc.intpending & SCC_RR3_CHA_EXT) && (scc.chan[SCC_CHANA].wr1 & 1)) {
		active = 1;
	}
	if ((scc.intpending & SCC_RR3_CHB_EXT) && (scc.chan[SCC_CHANB].wr1 & 1)) {
		active = 1;
	}
	// Musashi clears CPU_INT_LEVEL when it acknowledges an interrupt, so an
	// active SCC level must be re-presented until the SCC pending bit is reset.
	if (active) {
		scc.irqAsserted = 1;
		sccIrq(1);
	} else if (scc.irqAsserted) {
		scc.irqAsserted = 0;
		sccIrq(0);
	}
}

// Detect external line transitions that should raise SCC interrupts.
static void checkExtInt(int chan) {
	uint8_t rr0 = calcRr0(chan);
	uint8_t dif = rr0 ^ scc.chan[chan].rr0Prev;
	uint8_t wr15 = scc.chan[chan].wr15;
	int triggered = 0;

	if ((dif & SCC_R0_BREAKABRT) && (wr15 & SCC_WR15_BREAK)) triggered = 1;
	if ((dif & SCC_R0_CTS) && (wr15 & SCC_WR15_CTS)) triggered = 1;
	if ((dif & SCC_R0_DCD) && (wr15 & SCC_WR15_DCD)) triggered = 1;
	if ((dif & SCC_R0_SYNCHUNT) && (wr15 & SCC_WR15_SYNC)) triggered = 1;
	if ((dif & SCC_R0_EOM) && (wr15 & SCC_WR15_TXU)) triggered = 1;
	if ((dif & SCC_R0_ZEROCOUNT) && (wr15 & SCC_WR15_ZCOUNT)) triggered = 1;

	if (triggered) {
		scc.chan[chan].rr0Latched = rr0;
		scc.intpending |= rr3_mask_for_ext(chan);
	}
	scc.chan[chan].rr0Prev = rr0;
	assessIrq();
}

// Update one channel's DCD input and re-evaluate interrupt state.
void sccSetDcd(int chan, int val) {
	if (chan < 0 || chan > 1) {
		return;
	}
	scc.chan[chan].dcd = val ? 1 : 0;
	checkExtInt(chan);
}

// Stubbed receive hook kept only for source compatibility with the older code.
void sccRecv(int chan, uint8_t *data, int len, int delay) {
	(void)chan;
	(void)data;
	(void)len;
	(void)delay;
}

// Handle one SCC register write from the Mac ROM.
// The SCC uses an indirect register-pointer scheme, so the function first
// resolves the target register and then applies the write semantics.
void sccWrite(unsigned int addr, unsigned int val) {
	int chan = chan_from_addr(addr);
	uint8_t reg;

	if (addr & (1 << 2)) {
		reg = 8;
	} else {
		reg = scc.regptr & 0x0f;
		scc.regptr = 0;
	}

	switch (reg) {
	case 0:
		scc.regptr = val & 0x07;
		if ((val & 0x38) == 0x08) {
			scc.regptr |= 0x08;
		} else if ((val & 0x38) == 0x10) {
			scc.chan[chan].rr0Latched = -1;
			scc.intpending &= ~rr3_mask_for_ext(chan);
		}
		break;
	case 1:
		scc.chan[chan].wr1 = val;
		break;
	case 2:
		scc.wr2 = val;
		break;
	case 3:
		scc.chan[chan].hunting = (val & 0x10) ? 1 : 0;
		break;
	case 6:
		break;
	case 8:
		break;
	case 9:
		scc.wr9 = val;
		if ((val & 0xc0) == 0xc0) {
			sccInit();
		}
		break;
	case 15:
		scc.chan[chan].wr15 = val;
		break;
	default:
		break;
	}

	checkExtInt(chan);
}

// Handle one SCC register read from the Mac ROM.
// Reads return live or latched status depending on the selected register.
unsigned int sccRead(unsigned int addr) {
	int chan = chan_from_addr(addr);
	uint8_t reg;
	uint8_t val = 0;

	if (addr & (1 << 2)) {
		reg = 8;
	} else {
		reg = scc.regptr & 0x0f;
		scc.regptr = 0;
	}

	switch (reg) {
	case 0:
		if (scc.chan[chan].rr0Latched >= 0) {
			val = (uint8_t)scc.chan[chan].rr0Latched;
			scc.chan[chan].rr0Latched = -1;
		} else {
			val = calcRr0(chan);
		}
		break;
	case 1:
		val = 0x07;
		break;
	case 2:
		val = scc.wr2;
		if (chan == SCC_CHANB && scc.intpending) {
			int rsn = 0;
			if (scc.intpending & SCC_RR3_CHB_EXT) {
				rsn = 1;
				scc.intpending &= ~SCC_RR3_CHB_EXT;
			} else if (scc.intpending & SCC_RR3_CHA_EXT) {
				rsn = 5;
				scc.intpending &= ~SCC_RR3_CHA_EXT;
			}
			if (scc.wr9 & 0x10) {
				val = (scc.wr2 & ~0x70) | (rsn << 4);
			} else {
				val = (scc.wr2 & ~0x0e) | (rsn << 1);
			}
		}
		break;
	case 3:
		val = (chan == SCC_CHANA) ? scc.intpending : 0;
		break;
	case 8:
		val = 0;
		break;
	case 10:
		val = 0;
		break;
	case 15:
		val = scc.chan[chan].wr15;
		break;
	default:
		val = 0;
		break;
	}

	checkExtInt(chan);
	return val;
}

// Timing hook kept for interface compatibility; this trimmed SCC does not use it.
void sccTick(int cycles) {
	(void)cycles;
}

// Reset the trimmed SCC model to the default state expected by the ROM.
void sccInit() {
	memset(&scc, 0, sizeof(scc));
	scc.chan[SCC_CHANA].dcd = 1;
	scc.chan[SCC_CHANB].dcd = 1;
	scc.chan[SCC_CHANA].cts = 1;
	scc.chan[SCC_CHANB].cts = 1;
	scc.chan[SCC_CHANA].rr0Latched = -1;
	scc.chan[SCC_CHANB].rr0Latched = -1;
	scc.chan[SCC_CHANA].rr0Prev = calcRr0(SCC_CHANA);
	scc.chan[SCC_CHANB].rr0Prev = calcRr0(SCC_CHANB);
	sccIrq(0);
}
