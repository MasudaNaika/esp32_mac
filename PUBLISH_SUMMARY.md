# ESP32 Mac publish summary

![Made with Codex](images/made-with-codex-banner.png)

This document is the short publish note for the current implementation compared
with the fork source, `origin/main`. The older in-repo notes are useful history,
but this file reflects the current tree.

## Main changes from the fork source

- Build system moved from PlatformIO/Arduino to native ESP-IDF for the
  Waveshare ESP32-S3 Touch LCD board, with managed ESP-IDF components,
  partition layout, PSRAM/XIP settings, TinyUSB, NimBLE, and project build
  policy documented.
- Runtime ownership was reorganized: the Mac emulator and hot emulator work use
  core 1, while Wi-Fi, HTTP, NTP, display/console helpers, USB, and support
  tasks use core 0.
- LCD output was rebuilt around a 1 bpp Mac front buffer and RGB565 bounce
  conversion for the ST7701 panel. Console, boot status, runtime status, and
  VMU surface switching were added.
- BLE input became a browser-driven Web Bluetooth trackpad/keyboard UI, with
  host-side input queuing so BLE callbacks are separated from emulator state.
- Wi-Fi now persists across boot, stores working credentials, falls back to the
  setup AP `ESP32Mac-Setup`, and serves setup at `http://192.168.6.1/`.
  Station mode also exposes `http://esp32mac.local/`.
- Web UI files under `/web` provide the main BLE UI, settings editor, SD
  file manager, upload/download, and HTTP console/log view. A built-in upload
  page is served when `/web/index.html` is missing.
- `/setting.txt` became the central boot configuration for ROM, floppy,
  hard-disk, Wi-Fi, NTP, display, logging, turbo, and boot-info options.
- SD storage was expanded with `/fd`, `/hd`, and `/web` bootstrap
  directories, safe upload/path validation, mounted-image protection, and a
  shared file backend for HD/FD images.
- SCSI HD support now selects targets `hd0` through `hd6`; floppy support adds
  two PV Sony-backed drives, raw 400K/800K images, read/write fallback, guest
  eject handling, and live mount requests.
- USB storage mode lets the firmware hand the SD card to a PC as TinyUSB MSC
  while the emulator is stopped and disk images are closed.
- Emulator timing and performance work includes Mac Plus ROM patching, PSRAM
  Mac RAM, saved PRAM, improved VIA/SCC/RTC behavior, PWM audio, audio-sync
  aware turbo, direct-check `mmu` as the standard MMU, and the unified
  opcode/cycle dispatch table as the standard Musashi runtime table.
- Status display no longer shows FMMU/LMMU. Sound synchronization state is shown
  as `T-` when synchronized and `T@` when turbo/no-sync is active.
- Project assets were added, including publish images and 3D enclosure files.

See [PUBLISH_DETAILS.md](PUBLISH_DETAILS.md) for exact usage notes, settings,
Web UI behavior, USB storage mode, and implementation detail.
