/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

// Guest-visible RTC state.
// This struct models the serial command progress plus the 32-byte PRAM/RTC
// register image that the Mac ROM reads and writes through the VIA pins.
typedef struct {
	int lastClkVal;
	int pos;
	uint16_t cmd;
	uint8_t mem[32];
} Rtc;

static Rtc rtc;
static int32_t rtcTimezoneOffsetSeconds;

// Write one Macintosh-epoch timestamp into the guest-visible RTC bytes.
// The hardware exposes both a primary and shadow copy of the time registers.
static void rtcWriteMacTimeToMem(uint32_t macTime) {
	for (int i=0; i<4; i++) {
		rtc.mem[i] = (macTime >> (i * 8)) & 0xff;
		rtc.mem[i + 4] = rtc.mem[i]; //undocumented; mac needs it tho'.
	}
}

// Refresh the RTC bytes from the ESP32 host clock when that clock looks valid.
static void rtcRefreshHostTime() {
	time_t unixNow = time(NULL);
	if (unixNow < 1600000000) {
		return;
	}
	uint32_t macTime = (uint32_t)(unixNow + 2082844800UL + rtcTimezoneOffsetSeconds);
	rtcWriteMacTimeToMem(macTime);
}

// Advance the guest-visible RTC state once per emulated second.
void rtcTick() {
	rtcRefreshHostTime();
}

// Reassemble a Macintosh-epoch timestamp from four RTC bytes.
static uint32_t rtcReadMacTimeFromMem(int base) {
	uint32_t macTime = 0;
	for (int i=0; i<4; i++) {
		macTime |= ((uint32_t)rtc.mem[base + i]) << (i * 8);
	}
	return macTime;
}

// Push a Mac-side RTC timestamp back into the ESP32 host clock.
static void rtcSetHostTimeFromMac(uint32_t macTime) {
	int64_t unixTime = (int64_t)macTime - 2082844800LL - rtcTimezoneOffsetSeconds;
	if (unixTime < 1600000000LL) {
		printf("RTC: ignoring invalid Mac-set time 0x%08x\n", (unsigned int)macTime);
		return;
	}

	struct timeval tv = {
		.tv_sec = (time_t)unixTime,
		.tv_usec = 0,
	};
	if (settimeofday(&tv, NULL) == 0) {
		printf("RTC: host time set from Mac 0x%08x Unix=%lld tz=%ld\n",
			(unsigned int)macTime,
			(long long)unixTime,
			(long)rtcTimezoneOffsetSeconds);
	} else {
		printf("RTC: settimeofday failed for Mac time 0x%08x\n", (unsigned int)macTime);
	}
}

extern void saveRtcMem(char *mem);

// Emulate the serial RTC protocol on the VIA pins.
// Steps:
// 1. collect command bits on rising clock edges,
// 2. on reads, stream register bits back to the guest,
// 3. on writes, update RTC bytes and optionally commit host time.
int rtcCom(int en, int dat, int clk) {
	int ret=0;
	clk=clk?1:0;
	if (en) {
		rtc.pos=0;
		rtc.cmd=0;
	} else {
		if (clk!=rtc.lastClkVal && clk) {
			if (rtc.pos<8 || (rtc.pos<16 && ((rtc.cmd&0x8000)==0)) ) {
				//First 8 bits, or all 16 bits if write: accumulate data
				if (dat) rtc.cmd|=(1<<(15-rtc.pos));
			}
//			printf("RTC: clocktick %d, dataline %d, cmd %x\n", rtc.pos, dat, rtc.cmd);
			if (rtc.cmd&0x8000) { //read
				if (rtc.pos==8) {
					// Time registers are refreshed lazily right before the guest reads them.
					int addr = (rtc.cmd&0x7C00)>>10;
					if (addr < 8) rtcRefreshHostTime();
					rtc.cmd|=rtc.mem[addr];
//					printf("RTC: Read cmd %x val %x\n", rtc.cmd>>8, (rtc.cmd&0xff));
				}
				ret=((rtc.cmd&(1<<(15-rtc.pos)))?1:0);
			} else if (rtc.pos==15) {
//				printf("RTC: Write cmd %x\n", rtc.cmd>>8);
				int addr = (rtc.cmd&0x7C00)>>10;
				rtc.mem[addr]=rtc.cmd&0xff;
				if (addr < 4) {
					// Writes to the primary time copy also update the mirrored shadow bytes.
					for (int i=0; i<4; i++) rtc.mem[i+4]=rtc.mem[i];
					if (addr == 3) rtcSetHostTimeFromMac(rtcReadMacTimeFromMem(0));
				} else if (addr < 8) {
					if (addr == 7) {
						// Some ROM paths write the shadow copy first and commit on the last byte.
						for (int i=0; i<4; i++) rtc.mem[i]=rtc.mem[i+4];
						rtcSetHostTimeFromMac(rtcReadMacTimeFromMem(4));
					}
				}
				saveRtcMem((char*)rtc.mem);
			}
			rtc.pos++;
		}
	}
	rtc.lastClkVal=clk;
	return ret;
}

// Initialize the RTC model from the persisted 32-byte PRAM image.
void rtcInit(char *mem) {
	memcpy(rtc.mem, mem, 32);
}

// Update the timezone offset used when translating host time to Mac time.
void rtcSetTimezoneOffset(int32_t offsetSeconds) {
	rtcTimezoneOffsetSeconds = offsetSeconds;
	rtcRefreshHostTime();
}

// Seed the guest-visible RTC time directly, typically after NTP sync.
void rtcSetMacTime(uint32_t macTime) {
	rtcWriteMacTimeToMem(macTime);
	printf("RTC: set Mac time 0x%08x mem=%02x %02x %02x %02x\n",
		(unsigned int)macTime, rtc.mem[0], rtc.mem[1], rtc.mem[2], rtc.mem[3]);
	saveRtcMem((char*)rtc.mem);
}
