#ifndef TME_VMU_H
#define TME_VMU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VMU_SURFACE_MAIN = 0,
    VMU_SURFACE_ALTERNATE = 1,
    VMU_SURFACE_CONSOLE = 2,
    VMU_SURFACE_COUNT = 3,
} VmuSurface;

void vmuInit(void);
void vmuAttachMmu(void);
uint8_t *vmuGetFrontBuffer(void);
uint8_t *vmuGetMappedBuffer(VmuSurface surface);
void vmuSelectMacSurface(VmuSurface surface);
void vmuShowConsole(void);
void vmuShowMac(void);
bool vmuConsoleActive(void);

#ifdef __cplusplus
}
#endif

#endif
