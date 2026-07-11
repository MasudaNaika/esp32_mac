# ESP32 Mac publish details

This is the detailed publish note for the current implementation compared with
the fork source, `origin/main`. Existing docs in this repository may describe
older intermediate states; when they disagree with this file, the current code
and this file are the publish reference.

## Build and platform

- Target board: Waveshare ESP32-S3 Touch LCD.
- Build system: native ESP-IDF, replacing the fork's PlatformIO/Arduino setup.
- Main ESP-IDF files added: `CMakeLists.txt`, `main/CMakeLists.txt`,
  `main/idf_component.yml`, `sdkconfig.defaults`, and `partitions.csv`.
- Managed components include mDNS and TinyUSB. The local PWM audio component was
  added under `components/pwm_audio`.
- Flash layout now contains `nvs`, `factory`, `rom`, and `hd` partitions. SD is
  preferred at runtime, while flash ROM/HD partitions remain useful fallback
  media.
- ESP32-S3 PSRAM and XIP options are enabled for the emulator-heavy workload.

## Runtime task ownership

- Core 1 is reserved for the Mac emulator task and performance-critical emulator
  work.
- Core 0 handles Wi-Fi, HTTP, NTP, display/console helpers, USB support, and
  other support tasks.
- Wi-Fi and lwIP are configured for core 0. HTTP server work is also kept on
  core 0.

## SD layout

On boot, the firmware expects or creates these SD paths:

```text
/sd/setting.txt
/sd/wifilist.txt
/sd/web/
/sd/fd/
/sd/hd/
```

Common media paths:

- ROM: `/sd/macplus.rom` by default.
- Floppy images: usually under `/sd/fd/`.
- Hard-disk images: usually under `/sd/hd/`.
- Web UI files: copied from repository `web/` to device `/sd/web/`.

Relative paths in settings are rooted at `/sd`. For example `hd/hd.img` means
`/sd/hd/hd.img`.

## `setting.txt`

Current default content:

```text
server=pool.ntp.org
ssid=
password=
tz=+09:00
rom=macplus.rom
fd0=
fd1=
hd0=
hd1=
hd2=
hd3=
hd4=
hd5=
hd6=hd/hd.img
log=on
lcdflip=on
turbo=on
boot_info=on
```

Keys:

- `server`: NTP server.
- `ssid`, `password`: primary Wi-Fi credential. Successful credentials are also
  stored in `wifilist.txt`.
- `tz`: fixed timezone offset such as `+09:00` or `-05:30`.
- `rom`: Mac Plus ROM path.
- `fd0`, `fd1`: floppy image paths for drive 0 and drive 1.
- `hd0` through `hd6`: SCSI hard-disk image paths. Target ID 7 is the initiator,
  so device IDs 0 through 6 are available.
- `log`: serial/HTTP log output, `on` or `off`.
- `lcdflip`: LCD orientation, `on` or `off`.
- `turbo`: enables no-wait/turbo behavior when sound synchronization allows it,
  `on` or `off`.
- `boot_info`: boot console/status information, `on` or `off`.

## Wi-Fi and setup AP

Boot order:

1. Try `ssid` and `password` from `setting.txt`.
2. Try saved entries from `/sd/wifilist.txt`.
3. If station connection fails, start setup AP mode.

Setup AP:

- SSID: `ESP32Mac-Setup`
- URL: `http://192.168.6.1/`

Station mode:

- mDNS name: `esp32mac`
- URL: `http://esp32mac.local/`

NTP uses the existing Wi-Fi service and runs periodically rather than tearing
Wi-Fi down after time sync.

## Web UI usage

Copy the repository `web/` directory to `/sd/web/` on the SD card. If
`/sd/web/index.html` is missing, the firmware serves a small built-in upload
page so the Web UI can be restored from the browser.

Pages:

- `/` or `/index.html`: Web Bluetooth Mac input UI. It provides a trackpad,
  mouse buttons, virtual M0110A keyboard, sticky modifiers, caps lock, and
  connect/disconnect controls.
- `/setting` or `/setting.html`: settings editor for Wi-Fi, NTP, ROM, floppy,
  hard disk, logging, LCD flip, turbo, and boot-info values. It also offers
  Wi-Fi scan results and media choices reported by the firmware.
- `/files` or `/files.html`: SD file manager with browse, mkdir, rename, delete,
  download, and upload.
- `/console` or `/console.html`: HTTP console command runner and live log view.

Useful HTTP APIs:

- `PUT /upload?path=web/setting.html` uploads a file to `/sd/web/setting.html`.
- `GET /api/settings` reads settings.
- `POST /api/settings` writes settings.
- `GET /api/wifi/scan` scans nearby Wi-Fi networks.
- `GET /api/files` lists selectable ROM/FD/HD files.
- `GET /api/filer?path=...` browses SD files.
- `GET /api/filer/download?path=...` downloads a file.
- `POST /api/filer/mkdir`, `/api/filer/delete`, `/api/filer/rename` manage SD
  files.
- `POST /api/console` runs a console command.
- `GET /api/console/events` streams logs with Server-Sent Events.

Upload and filer paths are intentionally conservative. Traversal, malformed
paths, directory targets, and unsafe mounted-image operations are rejected.

Browser note: Web Bluetooth and some browser APIs may require a secure origin.
For local HTTP use, Chrome-based browsers may need `http://esp32mac.local`
allowed through the browser's insecure-origin-as-secure development flag.

## USB storage mode

USB storage mode gives the SD card to a host PC as TinyUSB Mass Storage.

Entry points:

- Serial or HTTP console command: `storage`
- Boot/runtime storage menu item: `usb storage`
- ROM recovery/menu path when available

Behavior:

- The emulator is stopped at a safe point.
- HD and FD images are flushed and closed.
- The app-side `/sd` mount is released.
- TinyUSB MSC plus CDC is installed.
- The PC sees the SD card as a removable storage device.
- CDC remains available for simple serial echo/status.
- The firmware does not auto-format the card when mounting fails.
- After the PC ejects/removes the storage device, the firmware schedules a
  reboot so normal app ownership of the SD card can resume.

Usage rule: eject/remove the USB storage device on the PC before returning to
normal emulator use.

## Console commands

Available console commands:

```text
help
cls
console
emu
storage
log on
log off
perf on
perf off
cpu on
cpu off
status
show wifi
set wifi SSID PASSWORD
connect wifi
reboot
```

When opcode profiling is compiled in, these commands are also available:

```text
opprof on
opprof off
opprof reset
opprof dump [n]
```

`status` reports ROM/settings state, log/perf/cpu flags, sound sync/turbo state,
and DRAM/PSRAM memory. It no longer reports FMMU/LMMU.

## BLE input

The Web UI talks to the firmware with Web Bluetooth.

- Device name: `MacPlus`
- Service UUID: `931e0100-6858-4e55-b637-c3cfdab5ef5f`
- Characteristic UUID: `931e0101-6858-4e55-b637-c3cfdab5ef5f`

Packet families:

- Mouse motion/button packets.
- Key down packets.
- Key up packets.

The firmware routes BLE events through an input host layer so browser callbacks
do not directly mutate emulator device state. Keys are released on disconnect or
focus loss where the browser UI can detect it.

## Storage, HD, and FD behavior

- SD files are the normal storage backend.
- Flash ROM and flash HD partitions remain fallback sources.
- SCSI hard disks can be configured on `hd0` through `hd6`.
- The default hard disk is `hd6=hd/hd.img`.
- A shared `storage_backend` handles file open, read/write, flush, close, and
  read-only fallback for image files.
- The PV Sony floppy path supports two drives, `fd0` and `fd1`.
- Raw 400K and 800K floppy images are supported.
- Guest eject and runtime mount requests are supported.
- Mounted image delete/rename operations are blocked by the Web filer.

## Emulator, display, and performance

- The Mac Plus ROM is patched for this hardware path, including display and
  Sony-driver integration.
- Mac RAM is allocated in PSRAM.
- PRAM is saved through NVS.
- The current `mmu` is the fast optimized direct-check MMU.
- The unified Musashi opcode/cycle dispatch table for performance.
- VIA, SCC, RTC, keyboard, and interrupt behavior were tuned for the ESP32-S3
  runtime.
- PWM audio is used for Mac sound output.
- Turbo behavior cooperates with sound synchronization.
- Status display shows `T-` when sound is synchronized and `T@` when turbo or
  no-sync behavior is active.

## Display and local UI

- The ST7701 display path uses a 1 bpp Mac-visible front buffer and RGB565
  bounce-buffer conversion for LCD scanout.
- The VMU layer manages main, alternate, and console display surfaces.
- Boot console and status output show Wi-Fi/AP URL, HTTP information, storage
  state, and emulator status.
- Storage selection can be performed from the device menu as well as the Web UI.

## File groups changed from the fork source

- Removed PlatformIO entry point and added native ESP-IDF project structure.
- Added Web UI assets in `web/`.
- Added settings, HTTP server, console, log, Wi-Fi/NTP, QR helper, USB storage,
  storage menu, storage backend, and PV floppy modules under `src/`.
- Added emulator monitor, VMU, MMU, and Musashi runtime table changes under
  `src/tme/`.
- Moved vendor LCD/touch/display helper files out of `docs/` and into
  `vendor_drivers/`.
- Added publish notes, images, and 3D enclosure assets.
