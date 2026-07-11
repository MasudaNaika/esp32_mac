#pragma once

#include <stdbool.h>

// Initialize and mount SD card (SDMMC 1-bit mode)
bool sdcardInit();

// Unmount SD card and release SDMMC pins
void sdcardDeinit();

// Check if SD card is mounted
bool sdcardMounted();
