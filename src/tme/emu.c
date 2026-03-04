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
#include "snd.h"
#include "mouse.h"
#include <stdbool.h>
#include <sys/time.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


unsigned char *macRom;
unsigned char *macRam;
unsigned char *vidMem;

#define MEMADDR_DUMMY_CACHE (void*)1

int rom_remap, video_remap=0, audio_remap=0, audio_volume=0, audio_en=0;

void m68k_instruction() {
	unsigned int pc=m68k_get_reg(NULL, M68K_REG_PC);
	printf("Mon: %x\n", pc);
	int ok=0;
	if (pc < 0x400000) {
		if (rom_remap) {
			ok=1;
		}
	} else if (pc >= 0x400000 && pc<0x500000) {
		ok=1;
	}
	if (!ok) return;
	pc&=0x1FFFF;
	if (pc==0x7DCC) printf("Mon: SCSIReadSectors\n");
	if (pc==0x7E4C) printf("Mon: SCSIReadSectors exit OK\n");
	if (pc==0x7E56) printf("Mon: SCSIReadSectors exit FAIL\n");
}

typedef uint8_t (*PeripAccessCb)(unsigned int address, int data, int isWrite);

uint8_t unhandledAccessCb(unsigned int address, int data, int isWrite) {
	unsigned int pc=m68k_get_reg(NULL, M68K_REG_PC);
	printf("Unhandled %s @ 0x%X! PC=0x%X\n", isWrite?"write":"read", address, pc);
	return 0xff;
}

uint8_t bogusReadCb(unsigned int address, int data, int isWrite) {
	if (isWrite) return 0;
	return address^(address>>8)^(address>>16);
}

uint8_t ncrAccessCb(unsigned int address, int data, int isWrite) {
	if (isWrite) {
		ncrWrite((address>>4)&0x7, (address>>9)&1, data);
		return 0;
	} else {
		return ncrRead((address>>4)&0x7, (address>>9)&1);
	}
}

uint8_t sscAccessCb(unsigned int address, int data, int isWrite) {
	if (isWrite) {
		sccWrite(address, data);
		return 0;
	} else {
		return sccRead(address);
	}
}

uint8_t iwmAccessCb(unsigned int address, int data, int isWrite) {
	if (isWrite) {
		iwmWrite((address>>9)&0xf, data);
		return 0;
	} else {
		return iwmRead((address>>9)&0xf);;
	}
}

uint8_t sccIackCb(unsigned int address, int data, int isWrite) {
	// SCC interrupt acknowledge - read returns vector from SCC
	if (!isWrite) return sccRead(address);
	return 0;
}

uint8_t scsiDmaCb(unsigned int address, int data, int isWrite) {
	// SCSI pseudo-DMA area - return 0 for now
	(void)address; (void)data; (void)isWrite;
	return 0;
}

uint8_t viaAccessCb(unsigned int address, int data, int isWrite) {
	if (isWrite) {
		viaWrite((address>>9)&0xf, data);
		return 0;
	} else {
		return viaRead((address>>9)&0xf);
	}
}


#define FLAG_RO (1<<0);

typedef struct {
	uint8_t *memAddr;
	union {
		PeripAccessCb cb;
		int flags;
	};
} MemmapEnt;

#define MEMMAP_ES 0x20000 //entry size
#define MEMMAP_MAX_ADDR 0x1000000
MemmapEnt memmap[MEMMAP_MAX_ADDR/MEMMAP_ES];

#define MMAP_RAM_PTR(ent, addr) &ent->memAddr[addr&(MEMMAP_ES-1)]

static void regenMemmap(int remapRom) {
	int i;
	for (i=0; i<MEMMAP_MAX_ADDR/MEMMAP_ES; i++) {
		memmap[i].memAddr=0;
		memmap[i].cb=unhandledAccessCb;
	}

	if (remapRom) {
		memmap[0].memAddr=macRom;
		memmap[0].flags=FLAG_RO;
		for (i=1; i<0x400000/MEMMAP_ES; i++) {
			memmap[i].memAddr=NULL;
			memmap[i].cb=bogusReadCb;
		}
	} else {
		for (i=0; i<0x400000/MEMMAP_ES; i++) {
			memmap[i].memAddr=&macRam[(i*MEMMAP_ES)&(TME_RAMSIZE-1)];
			memmap[i].flags=0;
		}
	}

	memmap[0x400000/MEMMAP_ES].memAddr=macRom;
	memmap[0x400000/MEMMAP_ES].flags=FLAG_RO;
	for (i=0x400000/MEMMAP_ES+1; i<0x500000/MEMMAP_ES; i++) {
		memmap[i].memAddr=0;
		memmap[i].cb=bogusReadCb;
	}

	// 0x500000-0x580000: Video memory (for large screen hack)
	for (i=TME_VIDMEM_BASE/MEMMAP_ES; i<(TME_VIDMEM_BASE+TME_VIDMEM_SIZE)/MEMMAP_ES; i++) {
		memmap[i].memAddr=&vidMem[(i-TME_VIDMEM_BASE/MEMMAP_ES)*MEMMAP_ES];
		memmap[i].flags=0;
	}

	for (i=0x580000/MEMMAP_ES; i<0x600000/MEMMAP_ES; i++) {
		memmap[i].memAddr=NULL;
		memmap[i].cb=ncrAccessCb;
	}

	for (i=0x600000/MEMMAP_ES; i<0x700000/MEMMAP_ES; i++) {
		memmap[i].memAddr=&macRam[(i*MEMMAP_ES)&(TME_RAMSIZE-1)];
		memmap[i].flags=0;
	}

	for (i=0x800000/MEMMAP_ES; i<0xC00000/MEMMAP_ES; i++) {
		memmap[i].memAddr=NULL;
		memmap[i].cb=sscAccessCb;
	}

	for (i=0xc00000/MEMMAP_ES; i<0xe00000/MEMMAP_ES; i++) {
		memmap[i].memAddr=NULL;
		memmap[i].cb=iwmAccessCb;
	}
	for (i=0xE80000/MEMMAP_ES; i<0xF00000/MEMMAP_ES; i++) {
		memmap[i].memAddr=NULL;
		memmap[i].cb=viaAccessCb;
	}
	// 0xF00000-0xF80000: SCC interrupt ack (phase read)
	for (i=0xF00000/MEMMAP_ES; i<0xF80000/MEMMAP_ES; i++) {
		memmap[i].memAddr=NULL;
		memmap[i].cb=sccIackCb;
	}
	// 0xF80000-0x1000000: SCSI pseudo-DMA / phase-read area
	for (i=0xF80000/MEMMAP_ES; i<0x1000000/MEMMAP_ES; i++) {
		memmap[i].memAddr=NULL;
		memmap[i].cb=scsiDmaCb;
	}
}

uint8_t *macFb[2], *macSnd[2];

static void ramInit() {
	printf("Using PSRAM as Mac RAM (direct memory-mapped)\n");
	macRam=(unsigned char*)heap_caps_malloc(TME_RAMSIZE, MALLOC_CAP_SPIRAM);
	assert(macRam);
	printf("Mac RAM allocated at %p (%d bytes)\n", macRam, TME_RAMSIZE);

	// Separate video memory for large screen hack (mapped at 0x500000-0x580000)
	vidMem=(unsigned char*)heap_caps_malloc(TME_VIDMEM_SIZE, MALLOC_CAP_SPIRAM);
	assert(vidMem);
	printf("Video RAM allocated at %p (%d bytes)\n", vidMem, TME_VIDMEM_SIZE);
	memset(vidMem, 0xFF, TME_VIDMEM_SIZE);  // white screen initially

	macFb[0]=&vidMem[TME_SCREENBUF];
	macFb[1]=&vidMem[TME_SCREENBUF_ALT];
	macSnd[0]=&macRam[TME_SNDBUF];
	macSnd[1]=&macRam[TME_SNDBUF_ALT];
	printf("Clearing ram...\n");
	for (int x=0; x<TME_RAMSIZE; x++) macRam[x]=rand();
}


const inline static MemmapEnt *getMmmapEnt(const unsigned int address) {
	if (address>=MEMMAP_MAX_ADDR) return &memmap[127];
	return &memmap[address/MEMMAP_ES];
}

unsigned int m68k_read_memory_8(unsigned int address) {
	const MemmapEnt *mmEnt=getMmmapEnt(address);
	if (mmEnt->memAddr) {
		uint8_t *p;
		p=(uint8_t*)MMAP_RAM_PTR(mmEnt, address);
		return *p;
	} else {
		return mmEnt->cb(address, 0, 0);
	}
}

unsigned int m68k_read_memory_16(unsigned int address) {
	const MemmapEnt *mmEnt=getMmmapEnt(address);
	if ((address&1)!=0) printf("%s: Unaligned access to %x!\n", __FUNCTION__, address);
	if (mmEnt->memAddr) {
		uint16_t *p;
		p=(uint16_t*)MMAP_RAM_PTR(mmEnt, address);
		return __builtin_bswap16(*p);
	} else {
		unsigned int ret;
		ret=mmEnt->cb(address, 0, 0)<<8;
		ret|=mmEnt->cb(address+1, 0, 0);
		return ret;
	}
}

unsigned int m68k_read_memory_32(unsigned int address) {
	uint16_t a=m68k_read_memory_16(address);
	uint16_t b=m68k_read_memory_16(address+2);
	return (a<<16)|b;
}

void m68k_write_memory_8(unsigned int address, unsigned int value) {
	const MemmapEnt *mmEnt=getMmmapEnt(address);
	if (mmEnt->memAddr) {
		uint8_t *p;
		p=(uint8_t*)MMAP_RAM_PTR(mmEnt, address);
		*p=value;
	} else {
		mmEnt->cb(address, value, 1);
	}
}

void m68k_write_memory_16(unsigned int address, unsigned int value) {
	const MemmapEnt *mmEnt=getMmmapEnt(address);
	if ((address&1)!=0) printf("%s: Unaligned access to %x!\n", __FUNCTION__, address);
	if (mmEnt->memAddr) {
		uint16_t *p;
		p=(uint16_t*)MMAP_RAM_PTR(mmEnt, address);
		*p=__builtin_bswap16(value);
	} else {
		mmEnt->cb(address, (value>>8)&0xff, 1);
		mmEnt->cb(address+1, (value>>0)&0xff, 1);
	}
}

void m68k_write_memory_32(unsigned int address, unsigned int value) {
	m68k_write_memory_16(address, value>>16);
	m68k_write_memory_16(address+2, value);
}

unsigned char *m68k_pcbase=NULL;

void m68k_pc_changed_handler_function(unsigned int address) {
	const MemmapEnt *mmEnt=getMmmapEnt(address);
	if (mmEnt->memAddr) {
		uint8_t *p;
		p=(uint8_t*)MMAP_RAM_PTR(mmEnt, address);
		m68k_pcbase=p-address;
	} else {
		printf("PC not in mem!\n");
		abort();
	}
}


void printFps() {
	struct timeval tv;
	static struct timeval oldtv;
	gettimeofday(&tv, NULL);
	if (oldtv.tv_sec!=0) {
		long msec=(tv.tv_sec-oldtv.tv_sec)*1000;
		msec+=(tv.tv_usec-oldtv.tv_usec)/1000;
		printf("Speed: %d%%\n", (int)(100000/msec));
	}
	oldtv.tv_sec=tv.tv_sec;
	oldtv.tv_usec=tv.tv_usec;
}

void tmeStartEmu(void *rom) {
	int ca1=0, ca2=0;
	int x, frame=0;
	int cyclesPerSec=0;
	macRom=(unsigned char*)rom;
	ramInit();
	rom_remap=1;
	regenMemmap(1);
	printf("Creating HD and registering it...\n");
	SCSIDevice *hd=hdCreate("hd.img");
	ncrRegisterDevice(6, hd);
	viaClear(VIA_PORTA, 0x7F);
	viaSet(VIA_PORTA, 0x80);
	viaClear(VIA_PORTA, 0xFF);
	viaSet(VIA_PORTB, (1<<3));
	sccInit();
	printf("Initializing m68k...\n");
	m68k_pc_changed_handler_function(0x0);
	m68k_init();
	printf("Setting CPU type and resetting...");
	m68k_set_cpu_type(M68K_CPU_TYPE_68000);
	m68k_pulse_reset();
	printf("Display init...\n");
	sndInit();
	dispInit();
	printf("Done! Running.\n");
	while(1) {
		for (x=0; x<8000000/60; x+=1000) {
			m68k_execute(1000);
			viaStep(100);
			sccTick(100);

			int r=mouseTick();
			if (r&MOUSE_BTN) viaClear(VIA_PORTB, (1<<3)); else viaSet(VIA_PORTB, (1<<3));
			if (r&MOUSE_QXB) viaClear(VIA_PORTB, (1<<4)); else viaSet(VIA_PORTB, (1<<4));
			if (r&MOUSE_QYB) viaClear(VIA_PORTB, (1<<5)); else viaSet(VIA_PORTB, (1<<5));
			sccSetDcd(SCC_CHANA, r&MOUSE_QXA);
			sccSetDcd(SCC_CHANB, r&MOUSE_QYA);

			if (x>(8000000/120) && sndDone()) break;
		}
		cyclesPerSec+=x;
		dispDraw(macFb[video_remap?1:0]);
		sndPush(macSnd[audio_remap?1:0], audio_en?audio_volume:0);
		vTaskDelay(1); // yield to prevent watchdog timeout
		frame++;
		ca1^=1;
		viaControlWrite(VIA_CA1, ca1);
		if (frame==59) {
			ca2^=1;
			viaControlWrite(VIA_CA2, ca2);
		}
		if (frame>=60) {
			ca2^=1;
			viaControlWrite(VIA_CA2, ca2);
			rtcTick();
			frame=0;
			printFps();
			printf("%d Hz\n", cyclesPerSec);
			cyclesPerSec=0;
		}
	}
}

void viaIrq(int req) {
	m68k_set_irq(req?1:0);
}

void sccIrq(int req) {
	m68k_set_irq(req?2:0);
}


void viaCbPortAWrite(unsigned int val) {
	static int writes=0;
	if ((writes++)==0) val=0x67;
	video_remap=(val&(1<<6))?1:0;
	rom_remap=(val&(1<<4))?1:0;
	audio_remap=(val&(1<<3))?1:0;
	audio_volume=(val&7);
	iwmSetHeadSel(val&(1<<5));
	regenMemmap(rom_remap);
}

void viaCbPortBWrite(unsigned int val) {
	int b;
	b=rtcCom(val&4, val&1, val&2);
	if (b) viaSet(VIA_PORTB, 1); else viaClear(VIA_PORTB, 1);
	audio_en=!(val&(1<<7));
}
