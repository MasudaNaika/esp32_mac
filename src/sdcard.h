#pragma once

#include <stdbool.h>

// Initialize and mount SD card (SDMMC 1-bit mode)
bool sdcardInit();

// Check if SD card is mounted
bool sdcardMounted();
