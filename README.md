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
- Display rotated 90° CW, 1:1 pixel mapping (640x480 patched ROM on 480x640)
- PRAM saved to NVS
- WiFi with captive portal config (WiFiManager) + mDNS (`macplus.local`)
- UDP mouse/keyboard input over WiFi (port 4444)
- Python input client with absolute mouse positioning (`tools/mac_input.py`)

## Flash Layout

| Name    | Offset     | Size    | Purpose            |
|---------|------------|---------|-------------------|
| factory | 0x10000    | 1.5MB   | Application        |
| rom     | 0x190000   | 128KB   | Mac Plus ROM       |
| hd      | 0x1B0000   | ~14.3MB | SCSI Hard Disk     |

## Setup

### 1. Build and flash firmware

```bash
pio run -t upload
```

### 2. Flash Mac Plus ROM

```bash
esptool.py write_flash 0x190000 macplus.rom
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
esptool.py write_flash 0x1B0000 hd.hd
```

### 4. WiFi setup

On first boot, the ESP32 creates an AP called **MacPlus-Setup**. Connect to it from your phone or laptop and use the captive portal to configure your WiFi network. Credentials are saved — subsequent boots auto-connect.

Once connected, the device advertises as `macplus.local` via mDNS.

### 5. Input client

```bash
python3 tools/mac_input.py [host] [port]
```

Defaults to `macplus.local:4444`. Move mouse over the window to position the Mac cursor (absolute mapping). Click to click. Keys are mapped to Mac M0110A scancodes.

Note: Anaconda Python may block UDP sockets — the script auto-detects this and uses a `/usr/bin/python3` helper.

## Credits

- [minimacplus](https://github.com/Spritetm/minimacplus) by Jeroen Domburg (Spritetm)
- [Musashi](https://github.com/kstenerud/Musashi) 68K emulator by Karl Stenerud
