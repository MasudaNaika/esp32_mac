#ifndef TME_MMU_H
#define TME_MMU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Backing buffers for the guest-visible address space.
    uint8_t *rom;
    uint8_t *ram;
    uint8_t *volatile fb[2];
    // Points at the active ROM-remap flag owned by the emulator core.
    int *romRemap;
} MmuContext;

// 24-bit 68000 address space helpers and the Mac Plus layout constants.
#define ADDR_24BIT_MASK           0x00FFFFFFu
#define BOOT_ROM_END              0x000FFFFFu
#define MAIN_RAM_END              0x003FFFFFu
#define OVERLAY_ROM_BASE          0x00400000u
#define OVERLAY_ROM_END           0x0043FFFFu
#define RAM_REMAP_BASE            0x00600000u
#define RAM_REMAP_END             0x007FFFFFu
#define ROM_OFFSET_MASK           0x0001FFFFu
#define RAM_REMAP_MASK            0x001FFFFFu

#define NCR_BASE                  0x00580000u
#define NCR_END                   0x005FFFFFu
#define SCC_BASE                  0x00800000u
#define SCC_END                   0x00BFFFFFu
#define IWM_BASE                  0x00C00000u
#define IWM_END                   0x00DFFFFFu
#define VIA_BASE                  0x00E80000u
#define VIA_END                   0x00EFFFFFu
#define PHASE_READ_BASE           0x00F00000u
#define PHASE_READ_END            0x00F7FFFFu

typedef struct {
    // Lightweight counters that let the shell report MMU hot-path usage.
    uint32_t pcChangeCount;
    uint32_t read8Count;
    uint32_t read16Count;
    uint32_t read32Count;
    uint32_t write8Count;
    uint32_t write16Count;
    uint32_t write32Count;
} MmuStats;

// Install the current ROM/RAM/framebuffer backing stores.
void mmuSetContext(const MmuContext *ctx);
// Redirect the two guest VRAM ranges after a VMU front-buffer swap.
void mmuSetVideoFramebuffers(uint8_t *fb0, uint8_t *fb1);
// Rebuild the memory map when the ROM overlay bit changes.
void mmuSetRomRemap(int remap);
// Clear the MMU counters.
void mmuResetStats(void);
// Return the current counters and reset them for the next sample window.
MmuStats mmuSnapshotStats(void);

// Musashi entry points for guest reads and writes.
unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
unsigned int m68k_read_memory_32(unsigned int address);
void m68k_write_memory_8(unsigned int address, unsigned int value);
void m68k_write_memory_16(unsigned int address, unsigned int value);
void m68k_write_memory_32(unsigned int address, unsigned int value);
// Update Musashi's cached PC base when execution jumps into a mapped region.
void m68k_pc_changed_handler_function(unsigned int address);

#ifdef __cplusplus
}
#endif

#endif
