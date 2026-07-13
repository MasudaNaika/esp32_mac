#include "vmu.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "mmu.h"
#include "tmeconfig.h"

_Static_assert(TME_VIDMEM_SIZE_ALT == TME_VIDMEM_SIZE,
               "VMU surfaces must have equal sizes");

// The LCD ISR always scans this fixed internal-RAM address. Surface switches
// copy pixels into it instead of changing the LCD source pointer.
static uint8_t *frontBuffer;
static uint8_t *captureBuffer;

// Each logical surface has a persistent PSRAM backing store. The active
// surface temporarily lives in frontBuffer; its off-screen copy is refreshed
// when another surface becomes active.
static uint8_t *offscreen[VMU_SURFACE_COUNT];

// Guest accesses to the two Mac VRAM address ranges use these pointers. One
// points to frontBuffer when that page is visible; the other points to PSRAM.
static uint8_t *mappedMain;
static uint8_t *mappedAlternate;

// Console owns the front buffer during boot. PA6 selects which Mac surface
// will become active immediately, or after the console is dismissed.
static volatile VmuSurface activeSurface = VMU_SURFACE_CONSOLE;
static volatile VmuSurface selectedMacSurface = VMU_SURFACE_ALTERNATE;
static bool initialized;
static bool mmuAttached;

// Recompute the two MMU backing pointers after a surface switch. Initialization
// happens before the emulator installs its MMU context, so publication is
// deferred until vmuAttachMmu().
static void updateMappedBuffers(void) {
    mappedMain = activeSurface == VMU_SURFACE_MAIN
        ? frontBuffer : offscreen[VMU_SURFACE_MAIN];
    mappedAlternate = activeSurface == VMU_SURFACE_ALTERNATE
        ? frontBuffer : offscreen[VMU_SURFACE_ALTERNATE];
    if (mmuAttached) {
        mmuSetVideoFramebuffers(mappedMain, mappedAlternate);
    }
}

static void activateSurface(VmuSurface next) {
    assert(initialized);
    assert(next >= VMU_SURFACE_MAIN && next < VMU_SURFACE_COUNT);
    if (next == activeSurface) {
        return;
    }

    // Preserve the outgoing surface first, then restore the incoming surface
    // at the fixed address scanned by the LCD ISR.
    memcpy(offscreen[activeSurface], frontBuffer, TME_VIDMEM_SIZE);
    memcpy(frontBuffer, offscreen[next], TME_VIDMEM_SIZE);
    activeSurface = next;
    updateMappedBuffers();
}

void vmuInit(void) {
    if (initialized) {
        return;
    }

    // Only the continuously scanned front buffer consumes internal RAM.
    frontBuffer = (uint8_t *)heap_caps_malloc(
        TME_VIDMEM_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(frontBuffer);
    memset(frontBuffer, 0xFF, TME_VIDMEM_SIZE);

    // Main, alternate, and console retain their inactive contents in PSRAM.
    for (int surface = 0; surface < VMU_SURFACE_COUNT; ++surface) {
        offscreen[surface] = (uint8_t *)tme_psram_aligned_alloc(
            TME_VIDMEM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        assert(offscreen[surface]);
        memset(offscreen[surface], 0xFF, TME_VIDMEM_SIZE);
    }
    captureBuffer = (uint8_t *)tme_psram_aligned_alloc(
        TME_VIDMEM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(captureBuffer);
    memset(captureBuffer, 0xFF, TME_VIDMEM_SIZE);

    initialized = true;
    updateMappedBuffers();
    printf("VMU: front=%p (%d bytes), off-screen main=%p alt=%p console=%p\n",
           frontBuffer, TME_VIDMEM_SIZE,
           offscreen[VMU_SURFACE_MAIN], offscreen[VMU_SURFACE_ALTERNATE],
           offscreen[VMU_SURFACE_CONSOLE]);
}

void vmuAttachMmu(void) {
    assert(initialized);
    mmuAttached = true;
    updateMappedBuffers();
}

uint8_t *vmuGetFrontBuffer(void) {
    assert(initialized);
    return frontBuffer;
}

uint8_t *vmuCaptureFrontBuffer(void) {
    assert(initialized);
    memcpy(captureBuffer, frontBuffer, TME_VIDMEM_SIZE);
    return captureBuffer;
}

uint8_t *vmuGetMappedBuffer(VmuSurface surface) {
    assert(initialized);
    if (surface == VMU_SURFACE_MAIN) {
        return mappedMain;
    }
    if (surface == VMU_SURFACE_ALTERNATE) {
        return mappedAlternate;
    }
    return activeSurface == VMU_SURFACE_CONSOLE
        ? frontBuffer : offscreen[VMU_SURFACE_CONSOLE];
}

void vmuSelectMacSurface(VmuSurface surface) {
    assert(surface == VMU_SURFACE_MAIN || surface == VMU_SURFACE_ALTERNATE);
    // VIA PA6 can change while the console is visible. Remember that choice,
    // but leave console in the front buffer until vmuShowMac() is called.
    selectedMacSurface = surface;
    if (activeSurface != VMU_SURFACE_CONSOLE) {
        activateSurface(surface);
    }
}

void vmuShowConsole(void) {
    activateSurface(VMU_SURFACE_CONSOLE);
}

void vmuShowMac(void) {
    activateSurface(selectedMacSurface);
}

bool vmuConsoleActive(void) {
    return activeSurface == VMU_SURFACE_CONSOLE;
}
