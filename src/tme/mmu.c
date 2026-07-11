#include "mmu.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_attr.h"

#include "ncr.h"
#include "iwm.h"
#include "pv_fdd.h"
#include "scc.h"
#include "tmeconfig.h"
#include "via.h"

#define PERIPH_READ 0
#define PERIPH_WRITE 1

#define MMU_HOT_ATTR IRAM_ATTR

extern uint8_t ncrAccessCb(unsigned int address, int data, int isWrite);
extern uint8_t sscAccessCb(unsigned int address, int data, int isWrite);
extern uint8_t iwmAccessCb(unsigned int address, int data, int isWrite);
extern uint8_t phaseReadCb(unsigned int address, int data, int isWrite);
extern uint8_t viaAccessCb(unsigned int address, int data, int isWrite);

static MmuContext gMmu = {0};
static MmuStats gStats = {0};
unsigned char *m68k_pcbase = NULL;
typedef struct {
    uint32_t ramStart;
    uint32_t ramEnd8;
    uint32_t ramEnd16;
    uint32_t ramEnd32;
    uint32_t lowRomEnd8;
    uint32_t lowRomEnd16;
    uint32_t lowRomEnd32;
} FastMapState;

static FastMapState gMap;

#ifndef EMU_MMU_STATS_ENABLED
#define EMU_MMU_STATS_ENABLED 0
#endif

#if EMU_MMU_STATS_ENABLED
#define MMU_STAT_INC(field) (++gStats.field)
#else
#define MMU_STAT_INC(field) ((void)0)
#endif

// Check whether an address points into the primary VRAM bank.
static inline bool isVram0Address8(uint32_t address) {
    return address >= TME_VIDMEM_BASE && address < (TME_VIDMEM_BASE + TME_VIDMEM_SIZE);
}
static inline bool isVram0Address16(uint32_t address) {
    return address >= TME_VIDMEM_BASE && address < (TME_VIDMEM_BASE + TME_VIDMEM_SIZE - 1);
}
static inline bool isVram0Address32(uint32_t address) {
    return address >= TME_VIDMEM_BASE && address < (TME_VIDMEM_BASE + TME_VIDMEM_SIZE - 3);
}

// Check whether an address points into the alternate VRAM bank.
static inline bool isVram1Address8(uint32_t address) {
    return address >= TME_VIDMEM_BASE_ALT && address < TME_VIDMEM_BASE;
}
static inline bool isVram1Address16(uint32_t address) {
    return address >= TME_VIDMEM_BASE_ALT && address < (TME_VIDMEM_BASE - 1);
}
static inline bool isVram1Address32(uint32_t address) {
    return address >= TME_VIDMEM_BASE_ALT && address < (TME_VIDMEM_BASE - 3);
}

// Check whether an address currently resolves to main RAM.
// The active window is rebuilt when the ROM overlay state changes.
static inline bool isMainRamAddress8(uint32_t address) {
    return address >= gMap.ramStart && address <= gMap.ramEnd8;
}
static inline bool isMainRamAddress16(uint32_t address) {
    return address >= gMap.ramStart && address <= gMap.ramEnd16;
}
static inline bool isMainRamAddress32(uint32_t address) {
    return address >= gMap.ramStart && address <= gMap.ramEnd32;
}

// Check whether an address hits any currently visible ROM window.
static inline bool isRomAddress8(uint32_t address) {
    return address <= gMap.lowRomEnd8
        || (address >= OVERLAY_ROM_BASE && address <= OVERLAY_ROM_END);
}
static inline bool isRomAddress16(uint32_t address) {
    return address <= gMap.lowRomEnd16
        || (address >= OVERLAY_ROM_BASE && address <= OVERLAY_ROM_END - 1);
}
static inline bool isRomAddress32(uint32_t address) {
    return address <= gMap.lowRomEnd32
        || (address >= OVERLAY_ROM_BASE && address <= OVERLAY_ROM_END - 3);
}

// Convert a logical RAM address into an offset inside the RAM buffer.
static inline uint32_t ramOffset(uint32_t address) {
    return address - gMap.ramStart;
}

static void rebuildFastMap(int bootOverlay) {
    if (bootOverlay) {
        gMap.ramStart = RAM_REMAP_BASE;
        gMap.ramEnd8 = RAM_REMAP_END;
        gMap.ramEnd16 = RAM_REMAP_END - 1;
        gMap.ramEnd32 = RAM_REMAP_END - 3;
        gMap.lowRomEnd8 = BOOT_ROM_END;
        gMap.lowRomEnd16 = BOOT_ROM_END - 1;
        gMap.lowRomEnd32 = BOOT_ROM_END - 3;
    } else {
        gMap.ramStart = 0;
        gMap.ramEnd8 = MAIN_RAM_END;
        gMap.ramEnd16 = MAIN_RAM_END - 1;
        gMap.ramEnd32 = MAIN_RAM_END - 3;
        gMap.lowRomEnd8 = 0;
        gMap.lowRomEnd16 = 0;
        gMap.lowRomEnd32 = 0;
    }
}

static inline uint16_t readBigEndian16(const uint8_t *bytes) {
    return __builtin_bswap16(*(const uint16_t *)bytes);
}

static inline void writeBigEndian16(uint8_t *bytes, uint16_t value) {
    *(uint16_t *)bytes = __builtin_bswap16(value);
}

static inline uint32_t readBigEndian32(const uint8_t *bytes) {
    if (((uintptr_t)bytes & 1) != 0) {
        return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
             | ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
    }
    const uint16_t *p = (const uint16_t *)bytes;
    uint16_t a = __builtin_bswap16(p[0]);
    uint16_t b = __builtin_bswap16(p[1]);
    return ((uint32_t)a << 16) | b;
}

static inline void writeBigEndian32(uint8_t *bytes, uint32_t value) {
    if (((uintptr_t)bytes & 1) != 0) {
        bytes[0] = value >> 24;
        bytes[1] = value >> 16;
        bytes[2] = value >> 8;
        bytes[3] = value;
        return;
    }
    uint16_t *p = (uint16_t *)bytes;
    p[0] = __builtin_bswap16(value >> 16);
    p[1] = __builtin_bswap16(value);
}

// Dispatch one peripheral byte read using direct address tests.
// This is the main fast-path replacement for the region-scan MMU.
static inline uint8_t readPeripheral8(uint32_t address) {
    if (address >= VIA_BASE && address <= VIA_END) {
        return viaAccessCb(address, 0, PERIPH_READ);
    }
    if (address >= NCR_BASE && address <= NCR_END) {
        return ncrAccessCb(address, 0, PERIPH_READ);
    }
    if (address >= SCC_BASE && address <= SCC_END) {
        return sscAccessCb(address, 0, PERIPH_READ);
    }
    if (address >= PHASE_READ_BASE && address <= PHASE_READ_END) {
        return phaseReadCb(address, 0, PERIPH_READ);
    }
    if (address >= IWM_BASE && address <= IWM_END) {
        return iwmAccessCb(address, 0, PERIPH_READ);
    }
    return 0xFF;
}

// Dispatch one peripheral byte write using the same fixed address windows.
static inline void writePeripheral8(uint32_t address, uint8_t value) {
    if (address >= VIA_BASE && address <= VIA_END) {
        viaAccessCb(address, value, PERIPH_WRITE);
        return;
    }
    if (address >= NCR_BASE && address <= NCR_END) {
        ncrAccessCb(address, value, PERIPH_WRITE);
        return;
    }
    if (address >= SCC_BASE && address <= SCC_END) {
        sscAccessCb(address, value, PERIPH_WRITE);
        return;
    }
    if (address == PV_FDD_GUEST_ADDR) {
        pvFddHandleWrite(value);
        return;
    }
    if (address >= PHASE_READ_BASE && address <= PHASE_READ_END) {
        phaseReadCb(address, value, PERIPH_WRITE);
        return;
    }
    if (address >= IWM_BASE && address <= IWM_END) {
        iwmAccessCb(address, value, PERIPH_WRITE);
        return;
    }
}

// Install the active backing buffers used by the direct-check MMU.
void mmuSetContext(const MmuContext *ctx) {
    gMmu = *ctx;
    rebuildFastMap(ctx->romRemap && *ctx->romRemap);
}

void mmuSetVideoFramebuffers(uint8_t *fb0, uint8_t *fb1) {
    gMmu.fb[0] = fb0;
    gMmu.fb[1] = fb1;
}

// Update the ROM overlay flag observed by the direct address predicates.
void mmuSetRomRemap(int remap) {
    *gMmu.romRemap = remap;
    rebuildFastMap(remap != 0);
}

// Clear the per-sample access counters.
void mmuResetStats(void) {
    gStats = (MmuStats){0};
}

// Return the current counter block and reset it for the next sample window.
MmuStats mmuSnapshotStats(void) {
    MmuStats stats = gStats;
    gStats = (MmuStats){0};
    return stats;
}

// Fast byte read path.
// Steps:
// 1. test ROM, RAM, and VRAM regions directly,
// 2. fall back to the peripheral decoder,
// 3. return 0xFF for unmapped 24-bit space.
unsigned int MMU_HOT_ATTR m68k_read_memory_8(unsigned int address) {
    MMU_STAT_INC(read8Count);
    if (isMainRamAddress8(address)) {
        return gMmu.ram[ramOffset(address)];
    }
    if (isRomAddress8(address)) {
        return gMmu.rom[address & ROM_OFFSET_MASK];
    }
    if (isVram0Address8(address)) {
        return gMmu.fb[0][address - TME_VIDMEM_BASE];
    }
    if (isVram1Address8(address)) {
        return gMmu.fb[1][address - TME_VIDMEM_BASE_ALT];
    }
    return address <= ADDR_24BIT_MASK ? readPeripheral8(address) : 0xFF;
}

// Fast 16-bit big-endian read path with direct address checks.
unsigned int MMU_HOT_ATTR m68k_read_memory_16(unsigned int address) {
    MMU_STAT_INC(read16Count);
    if ((address & 1) != 0) {
        printf("%s: Unaligned access to %x!\n", __FUNCTION__, address);
    }
    if (isMainRamAddress16(address)) {
        return readBigEndian16(gMmu.ram + ramOffset(address));
    }
    if (isRomAddress16(address)) {
        return readBigEndian16(gMmu.rom + (address & ROM_OFFSET_MASK));
    }
    if (isVram0Address16(address)) {
        return readBigEndian16(gMmu.fb[0] + (address - TME_VIDMEM_BASE));
    }
    if (isVram1Address16(address)) {
        return readBigEndian16(gMmu.fb[1] + (address - TME_VIDMEM_BASE_ALT));
    }
    return ((uint16_t)readPeripheral8(address) << 8) | (uint16_t)readPeripheral8(address + 1);
}

// Fast 32-bit big-endian read path.
// Common RAM, ROM, and VRAM cases stay inline to avoid extra dispatch overhead.
unsigned int MMU_HOT_ATTR m68k_read_memory_32(unsigned int address) {
    MMU_STAT_INC(read32Count);
    if (isMainRamAddress32(address)) {
        return readBigEndian32(gMmu.ram + ramOffset(address));
    }
    if (isRomAddress32(address)) {
        return readBigEndian32(gMmu.rom + (address & ROM_OFFSET_MASK));
    }
    if (isVram0Address32(address)) {
        return readBigEndian32(gMmu.fb[0] + (address - TME_VIDMEM_BASE));
    }
    if (isVram1Address32(address)) {
        return readBigEndian32(gMmu.fb[1] + (address - TME_VIDMEM_BASE_ALT));
    }
    return ((uint32_t)readPeripheral8(address) << 24)
         | ((uint32_t)readPeripheral8(address + 1) << 16)
         | ((uint32_t)readPeripheral8(address + 2) << 8)
         | (uint32_t)readPeripheral8(address + 3);
}

// Fast byte write path.
// Steps:
// 1. write directly into RAM or VRAM when possible,
// 2. otherwise forward to peripherals.
void MMU_HOT_ATTR m68k_write_memory_8(unsigned int address, unsigned int value) {
    MMU_STAT_INC(write8Count);
    if (isMainRamAddress8(address)) {
        gMmu.ram[ramOffset(address)] = value;
        return;
    }
    if (isVram0Address8(address)) {
        uint32_t off = address - TME_VIDMEM_BASE;
        gMmu.fb[0][off] = value;
        return;
    }
    if (isVram1Address8(address)) {
        uint32_t off = address - TME_VIDMEM_BASE_ALT;
        gMmu.fb[1][off] = value;
        return;
    }
    if (address <= ADDR_24BIT_MASK) {
        writePeripheral8(address, value);
    }
}

// Fast 16-bit big-endian write path for the common RAM and VRAM cases.
void MMU_HOT_ATTR m68k_write_memory_16(unsigned int address, unsigned int value) {
    MMU_STAT_INC(write16Count);
    if ((address & 1) != 0) {
        printf("%s: Unaligned access to %x!\n", __FUNCTION__, address);
    }
    if (isMainRamAddress16(address)) {
        writeBigEndian16(gMmu.ram + ramOffset(address), value);
        return;
    }
    if (isVram0Address16(address)) {
        writeBigEndian16(gMmu.fb[0] + (address - TME_VIDMEM_BASE), value);
        return;
    }
    if (isVram1Address16(address)) {
        writeBigEndian16(gMmu.fb[1] + (address - TME_VIDMEM_BASE_ALT), value);
        return;
    }
    writePeripheral8(address, (value >> 8) & 0xFF);
    writePeripheral8(address + 1, value & 0xFF);
}

// Fast 32-bit big-endian write path.
void MMU_HOT_ATTR m68k_write_memory_32(unsigned int address, unsigned int value) {
    MMU_STAT_INC(write32Count);
    if (isMainRamAddress32(address)) {
        writeBigEndian32(gMmu.ram + ramOffset(address), value);
        return;
    }
    if (isVram0Address32(address)) {
        writeBigEndian32(gMmu.fb[0] + (address - TME_VIDMEM_BASE), value);
        return;
    }
    if (isVram1Address32(address)) {
        writeBigEndian32(gMmu.fb[1] + (address - TME_VIDMEM_BASE_ALT), value);
        return;
    }
    writePeripheral8(address, (value >> 24) & 0xFF);
    writePeripheral8(address + 1, (value >> 16) & 0xFF);
    writePeripheral8(address + 2, (value >> 8) & 0xFF);
    writePeripheral8(address + 3, value & 0xFF);
}

// Refresh Musashi's cached PC base after a control-flow change.
// The fast MMU resolves the destination with direct region predicates.
void MMU_HOT_ATTR m68k_pc_changed_handler_function(unsigned int address) {
    MMU_STAT_INC(pcChangeCount);
    if (isMainRamAddress8(address)) {
        m68k_pcbase = (gMmu.ram + ramOffset(address)) - address;
        return;
    }
    if (isRomAddress8(address)) {
        m68k_pcbase = (gMmu.rom + (address & ROM_OFFSET_MASK)) - address;
        return;
    }
    if (isVram0Address8(address)) {
        m68k_pcbase = (gMmu.fb[0] + (address - TME_VIDMEM_BASE)) - address;
        return;
    }
    if (isVram1Address8(address)) {
        m68k_pcbase = (gMmu.fb[1] + (address - TME_VIDMEM_BASE_ALT)) - address;
        return;
    }
    printf("PC not in mem!\n");
    abort();
}
