#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Install the original PV floppy driver payload into a supported ROM.
bool pvSonyPatchRom(uint8_t *rom, size_t romSize);
