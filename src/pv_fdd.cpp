#include "pv_fdd.h"

#include <stdio.h>
#include <string.h>

#include "app_settings.h"
#include "console_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "storage_backend.h"

extern "C" {
#include "tme/musashi/m68k.h"
}

namespace {

constexpr size_t kFloppy400K = 400u * 1024u;
constexpr size_t kFloppy800K = 800u * 1024u;
constexpr int16_t kNoErr = 0;
constexpr int16_t kControlErr = -17;
constexpr int16_t kStatusErr = -18;
constexpr int16_t kReadErr = -19;
constexpr int16_t kWriteErr = -20;
constexpr int16_t kEofErr = -39;
constexpr int16_t kWriteProtectErr = -44;
constexpr int16_t kParamErr = -50;
constexpr int16_t kNoSuchDriveErr = -56;
constexpr int16_t kOfflineErr = -65;

constexpr uint32_t kDiskError = 0x0142;
constexpr uint32_t kTagBuffer = 0x02FC;
constexpr uint32_t kIoTrap = 6;
constexpr uint32_t kIoVRefNum = 22;
constexpr uint32_t kIoBuffer = 32;
constexpr uint32_t kIoReqCount = 36;
constexpr uint32_t kIoActCount = 40;
constexpr uint32_t kControlCode = 26;
constexpr uint32_t kControlParam = 28;
constexpr uint32_t kDceQueueFlags = 6;
constexpr uint32_t kDcePosition = 16;

constexpr uint32_t kStatusWriteProtected = 2;
constexpr uint32_t kStatusDiskInPlace = 3;
constexpr uint32_t kStatusInstalled = 4;
constexpr uint32_t kStatusSides = 5;
constexpr uint32_t kStatusQueueType = 10;
constexpr uint32_t kStatusDrive = 12;
constexpr uint32_t kStatusRefNum = 14;
constexpr uint32_t kStatusTwoSideFormat = 18;
constexpr size_t kImageNameSize = 128;

enum class MediaState : uint8_t {
    Empty,
    PendingInsert,
    Inserted,
    Accessed,
    Error,
};

struct ImageRequest {
    uint8_t drive;
    char configuredName[kImageNameSize];
};

struct PvFloppy {
    // Raw image bytes live behind StorageBackend; guest-visible floppy state
    // such as mediaState and Drive Status stays in this PV FDD layer.
    StorageBackend backend;
    size_t imageSize;
    uint32_t statusAddress;
    MediaState mediaState;
    char mountedName[kImageNameSize];
};

PvFloppy gFloppies[PV_FDD_DRIVE_COUNT] = {};
uint8_t *gGuestRam = nullptr;
size_t gGuestRamSize = 0;
bool gConfigured = false;
QueueHandle_t gImageRequests = nullptr;
portMUX_TYPE gMountedNameMux = portMUX_INITIALIZER_UNLOCKED;

void setMountedName(PvFloppy &floppy, const char *name) {
    taskENTER_CRITICAL(&gMountedNameMux);
    snprintf(floppy.mountedName, sizeof(floppy.mountedName), "%s", name ? name : "");
    taskEXIT_CRITICAL(&gMountedNameMux);
}

bool guestRange(uint32_t address, size_t size) {
    if (address > gGuestRamSize) {
        return false;
    }
    return size <= gGuestRamSize - address;
}

uint16_t read16(uint32_t address) {
    if (!guestRange(address, 2)) {
        return 0;
    }
    return (uint16_t)((uint16_t)gGuestRam[address] << 8)
         | (uint16_t)gGuestRam[address + 1];
}

uint32_t read32(uint32_t address) {
    if (!guestRange(address, 4)) {
        return 0;
    }
    return ((uint32_t)gGuestRam[address] << 24)
         | ((uint32_t)gGuestRam[address + 1] << 16)
         | ((uint32_t)gGuestRam[address + 2] << 8)
         | (uint32_t)gGuestRam[address + 3];
}

void write8(uint32_t address, uint8_t value) {
    if (guestRange(address, 1)) {
        gGuestRam[address] = value;
    }
}

void write16(uint32_t address, uint16_t value) {
    if (!guestRange(address, 2)) {
        return;
    }
    gGuestRam[address] = (uint8_t)(value >> 8);
    gGuestRam[address + 1] = (uint8_t)value;
}

void write32(uint32_t address, uint32_t value) {
    if (!guestRange(address, 4)) {
        return;
    }
    gGuestRam[address] = (uint8_t)(value >> 24);
    gGuestRam[address + 1] = (uint8_t)(value >> 16);
    gGuestRam[address + 2] = (uint8_t)(value >> 8);
    gGuestRam[address + 3] = (uint8_t)value;
}

int16_t finish(int16_t error) {
    write16(kDiskError, (uint16_t)error);
    m68k_set_reg(M68K_REG_D0, (uint32_t)(int32_t)error);
    return error;
}

void updateMediaStatus(PvFloppy &floppy) {
    if (!floppy.statusAddress) {
        return;
    }
    write8(floppy.statusAddress + kStatusWriteProtected,
           floppy.backend.readOnly ? 0xFF : 0x00);
    uint8_t diskInPlace = 0;
    if (floppy.backend.file) {
        diskInPlace = (floppy.mediaState == MediaState::Accessed ||
                       floppy.mediaState == MediaState::Error) ? 2 : 1;
    }
    write8(floppy.statusAddress + kStatusDiskInPlace, diskInPlace);
    write8(floppy.statusAddress + kStatusSides,
           floppy.imageSize == kFloppy800K ? 0xFF : 0x00);
    write8(floppy.statusAddress + kStatusTwoSideFormat,
           floppy.imageSize == kFloppy800K ? 0xFF : 0x00);
}

bool openImage(uint8_t drive, const char *configuredName) {
    if (drive >= PV_FDD_DRIVE_COUNT) {
        return false;
    }
    PvFloppy &floppy = gFloppies[drive];
    if (!configuredName || !configuredName[0]) {
        return true;
    }

    char path[160];
    const char *resolved = resolveSdConfigPath(configuredName, nullptr, path, sizeof(path));
    if (!resolved) {
        return false;
    }

    StorageBackend backend = {};
    // storage_backend handles open mode, read-only fallback, and size probing;
    // this layer keeps the floppy-specific 400K/800K raw image validation.
    if (!storageBackendOpenFile(&backend, resolved, true)) {
        consoleLogPrintf("FDD: cannot open %s\n", resolved);
        return false;
    }

    if (backend.size != kFloppy400K && backend.size != kFloppy800K) {
        consoleLogPrintf("FDD: unsupported raw image size %u for %s\n",
                         (unsigned)backend.size, resolved);
        storageBackendClose(&backend);
        return false;
    }

    floppy.backend = backend;
    floppy.imageSize = backend.size;
    floppy.mediaState = floppy.statusAddress
        ? MediaState::PendingInsert
        : MediaState::Inserted;
    setMountedName(floppy, configuredName);
    updateMediaStatus(floppy);
    consoleLogPrintf("FD%u: using %s (%ld bytes, %s)\n", (unsigned)drive,
                     resolved, (long)backend.size,
                     backend.readOnly ? "read-only" : "read/write");
    return true;
}

int driveIndex(uint32_t parameterBlock) {
    int driveNumber = (int16_t)read16(parameterBlock + kIoVRefNum);
    return driveNumber >= 1 && driveNumber <= PV_FDD_DRIVE_COUNT
        ? driveNumber - 1
        : -1;
}

int16_t handleOpen(uint32_t parameterBlock, uint32_t dce, uint32_t status) {
    (void)parameterBlock;
    constexpr size_t kAllStatusSize = 30u * PV_FDD_DRIVE_COUNT;
    if (!guestRange(status, kAllStatusSize) ||
        !guestRange(dce, kDcePosition + 4)) {
        return finish(kParamErr);
    }

    memset(gGuestRam + status, 0, kAllStatusSize);
    write16(dce + kDceQueueFlags,
            (uint16_t)((read16(dce + kDceQueueFlags) & 0xFF00u) | 3u));
    write32(dce + kDcePosition, 0);
    for (uint8_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
        PvFloppy &floppy = gFloppies[drive];
        uint32_t driveStatus = status + 30u * drive;
        floppy.statusAddress = driveStatus;
        write8(driveStatus + kStatusWriteProtected, floppy.backend.readOnly ? 0xFF : 0x00);
        write8(driveStatus + kStatusDiskInPlace, floppy.backend.file ? 1 : 0);
        write8(driveStatus + kStatusInstalled, 1);
        write8(driveStatus + kStatusSides,
               floppy.imageSize == kFloppy800K ? 0xFF : 0x00);
        write16(driveStatus + kStatusQueueType, 0);
        write16(driveStatus + kStatusDrive, drive + 1);
        write16(driveStatus + kStatusRefNum, (uint16_t)-5);
        write8(driveStatus + kStatusTwoSideFormat,
               floppy.imageSize == kFloppy800K ? 0xFF : 0x00);
        floppy.mediaState = floppy.backend.file
            ? MediaState::PendingInsert
            : MediaState::Empty;
        consoleLogPrintf("FD%u: guest drive opened, disk=%s blocks=%u sides=%u\n",
                         (unsigned)drive, floppy.backend.file ? "present" : "empty",
                         (unsigned)(floppy.imageSize / 512u),
                         floppy.imageSize == kFloppy800K ? 2u : 1u);
    }
    return finish(kNoErr);
}

int16_t handlePrime(uint32_t parameterBlock, uint32_t dce) {
    write32(parameterBlock + kIoActCount, 0);
    int drive = driveIndex(parameterBlock);
    if (drive < 0) {
        return finish(kNoSuchDriveErr);
    }
    PvFloppy &floppy = gFloppies[drive];
    if (!floppy.backend.file) {
        return finish(kOfflineErr);
    }

    uint32_t buffer = read32(parameterBlock + kIoBuffer);
    uint32_t length = read32(parameterBlock + kIoReqCount);
    uint32_t position = read32(dce + kDcePosition);
    if ((length & 0x1FFu) != 0 || (position & 0x1FFu) != 0 ||
        length > floppy.imageSize || position > floppy.imageSize - length ||
        !guestRange(buffer, length)) {
        return finish(kParamErr);
    }

    uint8_t command = (uint8_t)read16(parameterBlock + kIoTrap);
    if (appConsoleOutEnabled() && (command == 2 || command == 3)) {
        consoleLogPrintf("FD%d: %s %u bytes %s LBA %u.\n", drive,
                         command == 3 ? "Write" : "Read", (unsigned)length,
                         command == 3 ? "to" : "from", (unsigned)(position / 512u));
    }
    // After Device Manager parameter validation, Prime is only byte I/O plus
    // Mac OS error mapping. The backend does not know about guest RAM or DCEs.
    if (command == 2) {
        if (!storageBackendRead(&floppy.backend, position, gGuestRam + buffer, length)) {
            floppy.mediaState = MediaState::Error;
            updateMediaStatus(floppy);
            write32(parameterBlock + kIoActCount, 0);
            if (feof(floppy.backend.file)) {
                return finish(kEofErr);
            }
            return finish(kReadErr);
        }
    } else if (command == 3) {
        if (floppy.backend.readOnly) {
            return finish(kWriteProtectErr);
        }
        if (!storageBackendWrite(&floppy.backend, position, gGuestRam + buffer, length) ||
            !storageBackendFlush(&floppy.backend)) {
            floppy.mediaState = MediaState::Error;
            updateMediaStatus(floppy);
            write32(parameterBlock + kIoActCount, 0);
            return finish(kWriteErr);
        }
    } else {
        return finish(kParamErr);
    }

    write32(parameterBlock + kIoActCount, length);
    write32(dce + kDcePosition, position + length);
    if (command == 2 && guestRange(kTagBuffer, 12)) {
        memset(gGuestRam + kTagBuffer, 0, 12);
    }
    floppy.mediaState = MediaState::Accessed;
    updateMediaStatus(floppy);
    return finish(kNoErr);
}

int16_t handleControl(uint32_t parameterBlock) {
    uint16_t code = read16(parameterBlock + kControlCode);
    if (code == 65) {
        for (uint8_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
            PvFloppy &floppy = gFloppies[drive];
            if (floppy.mediaState == MediaState::PendingInsert) {
                floppy.mediaState = MediaState::Inserted;
                updateMediaStatus(floppy);
                m68k_set_reg(M68K_REG_D0, drive + 1);
                return drive + 1;
            }
        }
        return finish(kNoErr);
    }
    int drive = driveIndex(parameterBlock);
    if (drive < 0) {
        return finish(kNoSuchDriveErr);
    }
    PvFloppy &floppy = gFloppies[drive];

    switch (code) {
    case 1:
        return finish(-1);
    case 5:
        return finish(floppy.backend.file ? kNoErr : kOfflineErr);
    case 7:
        storageBackendClose(&floppy.backend);
        floppy.imageSize = 0;
        floppy.mediaState = MediaState::Empty;
        setMountedName(floppy, nullptr);
        updateMediaStatus(floppy);
        consoleLogPrintf("FD%d: guest ejected image\n", drive);
        return finish(kNoErr);
    case 8:
    case 9:
        return finish(kNoErr);
    case 23:
        // Report an internal or secondary external 800K GCR drive. Do not
        // advertise a SuperDrive: supported image formats are GCR-only.
        write32(parameterBlock + kControlParam,
                drive == 0 ? 0x00000003u : 0x00000903u);
        return finish(kNoErr);
    default:
        return finish(kControlErr);
    }
}

int16_t handleStatus(uint32_t parameterBlock) {
    int drive = driveIndex(parameterBlock);
    if (drive < 0) {
        return finish(kNoSuchDriveErr);
    }
    PvFloppy &floppy = gFloppies[drive];
    uint16_t code = read16(parameterBlock + kControlCode);
    if (code == 8) {
        uint32_t destination = parameterBlock + kControlParam;
        if (!floppy.statusAddress ||
            !guestRange(destination, 22) ||
            !guestRange(floppy.statusAddress, 22)) {
            return finish(kParamErr);
        }
        memcpy(gGuestRam + destination, gGuestRam + floppy.statusAddress, 22);
        return finish(kNoErr);
    }
    // Fixed-geometry format records cannot describe a 400K/800K GCR disk's
    // zone-dependent sector count. The stock-style behavior is to reject
    // this optional query instead of claiming MFM/SuperDrive geometry.
    return finish(kStatusErr);
}

} // namespace

bool pvFddInit(const char *configuredNames[PV_FDD_DRIVE_COUNT],
               uint8_t *guestRam, size_t guestRamSize) {
    pvFddShutdown();
    gGuestRam = guestRam;
    gGuestRamSize = guestRamSize;
    gImageRequests = xQueueCreate(PV_FDD_DRIVE_COUNT, sizeof(ImageRequest));
    if (!gImageRequests) {
        consoleLogPrintf("FDD: cannot create image request queue\n");
        pvFddShutdown();
        return false;
    }
    gConfigured = true;
    for (uint8_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
        const char *name = configuredNames ? configuredNames[drive] : nullptr;
        if (!name || !name[0]) {
            consoleLogPrintf("FD%u: PV drive enabled with no disk inserted\n", (unsigned)drive);
        } else if (!openImage(drive, name)) {
            consoleLogPrintf("FD%u: starting with an empty drive\n", (unsigned)drive);
        }
    }
    return true;
}

void pvFddShutdown(void) {
    for (PvFloppy &floppy : gFloppies) {
        storageBackendClose(&floppy.backend);
    }
    if (gImageRequests) {
        vQueueDelete(gImageRequests);
        gImageRequests = nullptr;
    }
    memset(gFloppies, 0, sizeof(gFloppies));
    gGuestRam = nullptr;
    gGuestRamSize = 0;
    gConfigured = false;
}

void pvFddHandleWrite(uint8_t operation) {
    if (!gConfigured || !gGuestRam) {
        return;
    }
    uint32_t parameterBlock = m68k_get_reg(nullptr, M68K_REG_A0) & 0x00FFFFFFu;
    uint32_t dce = m68k_get_reg(nullptr, M68K_REG_A1) & 0x00FFFFFFu;
    uint32_t status = m68k_get_reg(nullptr, M68K_REG_A2) & 0x00FFFFFFu;

    switch (operation) {
    case 0:
        handleOpen(parameterBlock, dce, status);
        break;
    case 1:
        handlePrime(parameterBlock, dce);
        break;
    case 2:
        handleControl(parameterBlock);
        break;
    case 3:
        handleStatus(parameterBlock);
        break;
    default:
        finish(kParamErr);
        break;
    }
}

bool pvFddConfigured(void) {
    return gConfigured;
}

bool pvFddRequestImage(uint8_t drive, const char *configuredName) {
    if (!gImageRequests || drive >= PV_FDD_DRIVE_COUNT) {
        return false;
    }
    ImageRequest request = {};
    request.drive = drive;
    snprintf(request.configuredName, sizeof(request.configuredName), "%s",
             configuredName ? configuredName : "");
    return xQueueSend(gImageRequests, &request, 0) == pdPASS;
}

void pvFddProcessRequests(void) {
    if (!gImageRequests) {
        return;
    }
    ImageRequest request;
    if (xQueueReceive(gImageRequests, &request, 0) != pdPASS) {
        return;
    }
    if (request.drive >= PV_FDD_DRIVE_COUNT) {
        return;
    }
    PvFloppy &floppy = gFloppies[request.drive];
    if (floppy.backend.file) {
        consoleLogPrintf("FD%u: eject the mounted disk in the guest before changing images\n",
                         (unsigned)request.drive);
        return;
    }
    if (request.configuredName[0]) {
        openImage(request.drive, request.configuredName);
    }
}

void pvFddGetMountedName(uint8_t drive, char *destination, size_t destinationSize) {
    if (drive >= PV_FDD_DRIVE_COUNT || !destination || !destinationSize) {
        return;
    }
    taskENTER_CRITICAL(&gMountedNameMux);
    snprintf(destination, destinationSize, "%s", gFloppies[drive].mountedName);
    taskEXIT_CRITICAL(&gMountedNameMux);
}
