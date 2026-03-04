# Mac Plus Emulator on ESP32-S3

A Macintosh Plus emulator running on the **Waveshare ESP32-S3-Touch-LCD-2.8B** board.

Based on [Spritetm's minimacplus](https://github.com/Spritetm/minimacplus), adapted for ESP32-S3 with OPI PSRAM and a 480x640 ST7701 RGB display.

## Hardware

- **Board:** Waveshare ESP32-S3-Touch-LCD-2.8B
- **CPU:** ESP32-S3 dual-core 240MHz
- **RAM:** 512KB SRAM + 8MB OPI PSRAM
- **Flash:** 16MB
- **Display:** 480x640 ST7701 RGB LCD

## Features

- Musashi 68000 CPU emulator running at ~4MHz (~50% of native 7.8MHz)
- 4MB Mac RAM in OPI PSRAM (direct memory-mapped, no cache layer)
- NCR 5380 SCSI with HD from flash partition
- VIA 6522, Zilog 8530 SCC, IWM (stub)
- Display rotated 90° CW, 1:1 pixel mapping (342x512 centered on 480x640)
- PRAM saved to NVS

## Flash Layout

| Name    | Offset     | Size    | Purpose            |
|---------|------------|---------|-------------------|
| factory | 0x10000    | 1MB     | Application        |
| rom     | 0x110000   | 128KB   | Mac Plus ROM       |
| hd      | 0x130000   | ~14.8MB | SCSI Hard Disk     |

## Setup

### 1. Build and flash firmware

```bash
pio run -t upload
```

### 2. Flash Mac Plus ROM

```bash
esptool.py write_flash 0x110000 macplus.rom
```

### 3. Create and flash HD image

A proper SCSI HD image with Apple partition map and driver is required.
Raw HFS/floppy images (from Mini vMac, etc.) will NOT work.

Use MAME to create one:

```bash
# Create blank HD (1.4MB, 2800 blocks)
dd if=/dev/zero of=hd.hd bs=512 count=2800

# Boot MAME with System floppy + blank HD
mame macplus -window -flop1 system6.dc42 -hard1 hd.hd

# In MAME: use Apple HD SC Setup to initialize, then install System
# Quit MAME, then flash:
esptool.py write_flash 0x130000 hd.hd
```

## Credits

- [minimacplus](https://github.com/Spritetm/minimacplus) by Jeroen Domburg (Spritetm)
- [Musashi](https://github.com/kstenerud/Musashi) 68K emulator by Karl Stenerud
