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

#include "freertos/FreeRTOS.h"

#include "input_host.h"
#include "via.h"

#define KBD_NULL 0x7B
#define KBD_MODEL 0x0B
#define KBD_TEST_ACK 0x7D

#define SR_TRANSFER_TICKS 8
#define KBD_INQUIRY_WAIT_TICKS 4000

void viaCbPortAWrite(unsigned int val);
void viaCbPortBWrite(unsigned int val);
void viaIrq(int req);

#define IFR_IRQ (1 << 7)
#define IFR_T1  (1 << 6)
#define IFR_T2  (1 << 5)
#define IFR_CB1 (1 << 4)
#define IFR_CB2 (1 << 3)
#define IFR_SR  (1 << 2)
#define IFR_CA1 (1 << 1)
#define IFR_CA2 (1 << 0)

#define PCR_NEG         0
#define PCR_NEG_NOCLR   1
#define PCR_POS         2
#define PCR_POS_NOCLR   3
#define PCR_HANDSHAKE   4
#define PCR_PULSEOUT    5
#define PCR_MAN_LO      6
#define PCR_MAN_HI      7

typedef enum {
	SR_IDLE,
	SR_TX_ACTIVE,
	SR_WAIT_FOR_RX_MODE,
	SR_RX_ACTIVE,
} SrPhase;

typedef struct {
	uint8_t ddra, ddrb;
	uint8_t ina, inb;
	uint8_t outa, outb;
	int32_t timer1, timer2;
	uint16_t latch1, latch2;
	uint8_t ifr, ier;
	uint8_t pcr, acr, sr;
	uint8_t controlin[4];
	uint8_t timer1Pb7;
	uint8_t kbdResponse;
	uint8_t kbdInstantResponse;
	uint8_t kbdInquiryPending;
	uint8_t irqAsserted;
	int srTicks;
	SrPhase srPhase;
	int timer1Active;
	int timer2Running;
	int timer2IrqArmed;
} Via;

static Via via;

static uint8_t kbdResponseForEvent(const InputHostKeyEvent *event) {
	if (event->scancode < 64) {
		return (uint8_t)((event->scancode << 1) |
			(event->isRelease ? 0x80 : 0));
	}
	via.kbdInstantResponse = (uint8_t)(((event->scancode - 64) << 1) |
		(event->isRelease ? 0x80 : 0));
	return 0x79;
}

static void viaCheckIrq(void) {
	int pending = (via.ifr & via.ier) & 0x7F;
	if (pending) {
		via.ifr |= IFR_IRQ;
	} else {
		via.ifr &= (uint8_t)~IFR_IRQ;
	}
	// Musashi's default interrupt acknowledge clears CPU_INT_LEVEL. Re-present
	// a level-triggered VIA IRQ while any enabled source remains pending.
	if (pending) {
		via.irqAsserted = 1;
		viaIrq(1);
	} else if (via.irqAsserted) {
		via.irqAsserted = 0;
		viaIrq(0);
	}
}

static uint8_t viaPortAValue(void) {
	return (via.outa & via.ddra) | (via.ina & (uint8_t)~via.ddra);
}

static uint8_t viaPortBValue(void) {
	uint8_t output = via.outb;
	if (via.acr & 0x80) {
		output = (output & 0x7F) | (via.timer1Pb7 ? 0x80 : 0);
	}
	return (output & via.ddrb) | (via.inb & (uint8_t)~via.ddrb);
}

static void viaNotifyPortA(void) {
	viaCbPortAWrite(via.outa);
}

static void viaNotifyPortB(void) {
	uint8_t output = via.outb;
	if (via.acr & 0x80) {
		output = (output & 0x7F) | (via.timer1Pb7 ? 0x80 : 0);
	}
	viaCbPortBWrite(output);
}

void viaInit(void) {
	memset(&via, 0, sizeof(via));
	via.kbdResponse = KBD_NULL;
	via.kbdInstantResponse = KBD_NULL;
	inputHostReset();
}

void viaSet(int no, int mask) {
	if (no == VIA_PORTA) {
		via.ina |= (uint8_t)mask;
	} else {
		via.inb |= (uint8_t)mask;
	}
}

void viaClear(int no, int mask) {
	if (no == VIA_PORTA) {
		via.ina &= (uint8_t)~mask;
	} else {
		via.inb &= (uint8_t)~mask;
	}
}

static uint8_t kbdResponseForCommand(uint8_t command) {
	switch (command) {
	case 0x10: { /* Inquiry */
		InputHostKeyEvent event;
		if (!inputHostPopKeyEvent(&event)) {
			via.kbdInquiryPending = 1;
			return KBD_NULL;
		}
		via.kbdInquiryPending = 0;
		return kbdResponseForEvent(&event);
	}
	case 0x14: { /* Instant */
		via.kbdInquiryPending = 0;
		uint8_t response = via.kbdInstantResponse;
		via.kbdInstantResponse = KBD_NULL;
		return response;
	}
	case 0x16: /* Model */
		via.kbdInquiryPending = 0;
		return KBD_MODEL;
	case 0x36: /* Test */
		via.kbdInquiryPending = 0;
		return KBD_TEST_ACK;
	case 0x00:
		via.kbdInquiryPending = 0;
		return 0;
	default:
		via.kbdInquiryPending = 0;
		return 0;
	}
}

static void viaStartSrReceive(void) {
	if (via.srPhase == SR_WAIT_FOR_RX_MODE) {
		// ACR switching to receive starts a new transfer. Do not let the ROM
		// mistake the previous transmit-complete flag for receive completion.
		via.ifr &= (uint8_t)~IFR_SR;
		viaCheckIrq();
		via.srPhase = SR_RX_ACTIVE;
		if (via.kbdInquiryPending) {
			InputHostKeyEvent event;
			if (inputHostPopKeyEvent(&event)) {
				via.kbdResponse = kbdResponseForEvent(&event);
				via.kbdInquiryPending = 0;
				via.srTicks = SR_TRANSFER_TICKS;
			} else {
				via.srTicks = KBD_INQUIRY_WAIT_TICKS;
			}
		} else {
			via.srTicks = SR_TRANSFER_TICKS;
		}
	}
}

static void kbdFillPendingInquiry(void) {
	if (!via.kbdInquiryPending || via.srPhase != SR_RX_ACTIVE) {
		return;
	}

	InputHostKeyEvent event;
	if (inputHostPopKeyEvent(&event)) {
		via.kbdResponse = kbdResponseForEvent(&event);
		via.kbdInquiryPending = 0;
		if (via.srTicks > SR_TRANSFER_TICKS) {
			via.srTicks = SR_TRANSFER_TICKS;
		}
	}
}

static void viaStepTimer1(int clockcycles) {
	if (!via.timer1Active) {
		return;
	}

	via.timer1 -= clockcycles;
	if (via.timer1 > 0) {
		return;
	}

	via.ifr |= IFR_T1;
	if (via.acr & 0x40) {
		int32_t period = via.latch1 ? via.latch1 : 0x10000;
		uint32_t underflows = 1u + (uint32_t)(-via.timer1) / (uint32_t)period;
		via.timer1 += (int32_t)(underflows * (uint32_t)period);
		if ((via.acr & 0x80) && (underflows & 1u)) {
			via.timer1Pb7 ^= 1;
			viaNotifyPortB();
		}
	} else {
		via.timer1Active = 0;
		if (via.acr & 0x80) {
			via.timer1Pb7 = 1;
			viaNotifyPortB();
		}
	}
}

static void viaStepTimer2(int clockcycles) {
	if (!via.timer2Running) {
		return;
	}

	via.timer2 -= clockcycles;
	if (via.timer2 <= 0) {
		if (via.timer2IrqArmed) {
			via.ifr |= IFR_T2;
			via.timer2IrqArmed = 0;
		}
		do {
			via.timer2 += 0x10000;
		} while (via.timer2 <= 0);
	}
}

static void viaStepShiftRegister(int clockcycles) {
	if (via.srTicks <= 0) {
		return;
	}

	kbdFillPendingInquiry();
	via.srTicks -= clockcycles;
	if (via.srTicks > 0) {
		return;
	}

	via.srTicks = 0;
	if (via.srPhase == SR_TX_ACTIVE) {
		via.srPhase = SR_WAIT_FOR_RX_MODE;
		via.ifr |= IFR_SR;
	} else if (via.srPhase == SR_RX_ACTIVE) {
		via.sr = via.kbdResponse;
		via.kbdInquiryPending = 0;
		via.srPhase = SR_IDLE;
		via.ifr |= IFR_SR;
	}
}

void viaStep(int clockcycles) {
	if (clockcycles <= 0) {
		return;
	}
	viaStepTimer1(clockcycles);
	viaStepTimer2(clockcycles);
	viaStepShiftRegister(clockcycles);
	viaCheckIrq();
}

static int pcrFor(int no) {
	switch (no) {
	case VIA_CA1:
		return (via.pcr & 0x01) ? PCR_POS : PCR_NEG;
	case VIA_CA2:
		return (via.pcr >> 1) & 0x07;
	case VIA_CB1:
		return (via.pcr & 0x10) ? PCR_POS : PCR_NEG;
	case VIA_CB2:
		return (via.pcr >> 5) & 0x07;
	default:
		return PCR_MAN_HI;
	}
}

static void accessPort(int no) {
	int control2 = no ? VIA_CB2 : VIA_CA2;
	int mode2 = pcrFor(control2);
	via.ifr &= (uint8_t)~(no ? IFR_CB1 : IFR_CA1);
	if (mode2 == PCR_NEG || mode2 == PCR_POS) {
		via.ifr &= (uint8_t)~(no ? IFR_CB2 : IFR_CA2);
	}
	viaCheckIrq();
}

void viaControlWrite(int no, int val) {
	static const uint8_t ifbits[] = { IFR_CA1, IFR_CA2, IFR_CB1, IFR_CB2 };
	if (no < VIA_CA1 || no > VIA_CB2) {
		return;
	}

	val = val ? 1 : 0;
	int mode = pcrFor(no);
	if (via.controlin[no] != val) {
		if (((mode == PCR_NEG || mode == PCR_NEG_NOCLR) && !val) ||
		    ((mode == PCR_POS || mode == PCR_POS_NOCLR) && val)) {
			via.ifr |= ifbits[no];
			viaCheckIrq();
		}
	}
	via.controlin[no] = (uint8_t)val;
}

void viaWrite(unsigned int addr, unsigned int val) {
	uint8_t data = (uint8_t)val;
	switch (addr & 0x0F) {
	case 0x0:
		via.outb = data;
		viaNotifyPortB();
		accessPort(1);
		break;
	case 0x1:
		via.outa = data;
		viaNotifyPortA();
		accessPort(0);
		break;
	case 0x2:
		via.ddrb = data;
		break;
	case 0x3:
		via.ddra = data;
		break;
	case 0x4:
		via.latch1 = (via.latch1 & 0xFF00) | data;
		break;
	case 0x5:
		via.latch1 = (via.latch1 & 0x00FF) | ((uint16_t)data << 8);
		via.timer1 = via.latch1 ? via.latch1 : 0x10000;
		via.timer1Active = 1;
		via.timer1Pb7 = 0;
		via.ifr &= (uint8_t)~IFR_T1;
		if (via.acr & 0x80) {
			viaNotifyPortB();
		}
		viaCheckIrq();
		break;
	case 0x6:
		via.latch1 = (via.latch1 & 0xFF00) | data;
		break;
	case 0x7:
		via.latch1 = (via.latch1 & 0x00FF) | ((uint16_t)data << 8);
		via.ifr &= (uint8_t)~IFR_T1;
		viaCheckIrq();
		break;
	case 0x8:
		via.latch2 = data;
		break;
	case 0x9:
		via.latch2 = (via.latch2 & 0x00FF) | ((uint16_t)data << 8);
		via.timer2 = via.latch2 ? via.latch2 : 0x10000;
		via.timer2Running = 1;
		via.timer2IrqArmed = 1;
		via.ifr &= (uint8_t)~IFR_T2;
		viaCheckIrq();
		break;
	case 0xA: {
		via.sr = data;
		via.ifr &= (uint8_t)~IFR_SR;
		int mode = (via.acr >> 2) & 0x07;
		if (mode == 7) {
			via.kbdResponse = kbdResponseForCommand(data);
			via.srPhase = SR_TX_ACTIVE;
			via.srTicks = SR_TRANSFER_TICKS;
		} else if (mode == 6) {
			via.kbdInquiryPending = 0;
			via.srPhase = SR_IDLE;
			via.srTicks = 0;
		}
		viaCheckIrq();
		break;
	}
	case 0xB: {
		uint8_t oldAcr = via.acr;
		via.acr = data;
		if (((oldAcr ^ data) & 0x80) != 0) {
			viaNotifyPortB();
		}
		if (((data >> 2) & 0x07) == 3) {
			viaStartSrReceive();
		}
		break;
	}
	case 0xC:
		via.pcr = data;
		break;
	case 0xD:
		via.ifr &= (uint8_t)~(data & 0x7F);
		viaCheckIrq();
		break;
	case 0xE:
		if (data & 0x80) {
			via.ier |= data & 0x7F;
		} else {
			via.ier &= (uint8_t)~(data & 0x7F);
		}
		viaCheckIrq();
		break;
	case 0xF:
		via.outa = data;
		viaNotifyPortA();
		break;
	}
}

unsigned int viaRead(unsigned int addr) {
	uint8_t val = 0;
	switch (addr & 0x0F) {
	case 0x0:
		val = viaPortBValue();
		accessPort(1);
		break;
	case 0x1:
		val = viaPortAValue();
		accessPort(0);
		break;
	case 0x2:
		val = via.ddrb;
		break;
	case 0x3:
		val = via.ddra;
		break;
	case 0x4:
		val = (uint8_t)(via.timer1 & 0xFF);
		via.ifr &= (uint8_t)~IFR_T1;
		viaCheckIrq();
		break;
	case 0x5:
		val = (uint8_t)((via.timer1 >> 8) & 0xFF);
		break;
	case 0x6:
		val = (uint8_t)(via.latch1 & 0xFF);
		break;
	case 0x7:
		val = (uint8_t)(via.latch1 >> 8);
		break;
	case 0x8:
		val = (uint8_t)(via.timer2 & 0xFF);
		via.ifr &= (uint8_t)~IFR_T2;
		viaCheckIrq();
		break;
	case 0x9:
		val = (uint8_t)((via.timer2 >> 8) & 0xFF);
		break;
	case 0xA:
		val = via.sr;
		via.ifr &= (uint8_t)~IFR_SR;
		viaCheckIrq();
		break;
	case 0xB:
		val = via.acr;
		break;
	case 0xC:
		val = via.pcr;
		break;
	case 0xD:
		val = via.ifr;
		break;
	case 0xE:
		val = via.ier | 0x80;
		break;
	case 0xF:
		val = viaPortAValue();
		break;
	}
	return val;
}
