#include <stdint.h>

// Refresh the guest-visible RTC registers from host time once per second.
void rtcTick();
// Bit-serial Mac RTC access handler on the VIA pins.
int rtcCom(int en, int dat, int clk);
// Restore the 32-byte RTC/PRAM state blob.
void rtcInit(char *mem);
// Update the host clock from a Mac-side timestamp.
void rtcSetMacTime(uint32_t macTime);
// Refresh the Mac-local timezone offset used by RTC conversions.
void rtcSetTimezoneOffset(int32_t offsetSeconds);
