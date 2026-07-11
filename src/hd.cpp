/*
 * SCSI HD emulation — SD card file (/sd/hd.img) or flash partition fallback.
 * SD card gives true read/write; flash partition requires erase-before-write.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_partition.h"
#include "app_settings.h"
#include "console_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "sdcard.h"
#include "storage_backend.h"

extern "C" {
#include "tme/ncr.h"
#include "tme/hd.h"
}

static constexpr bool kHdTrace = true;

typedef struct {
    StorageBackend fileBackend;
    const esp_partition_t* part;
    int size;
    char mountedPath[160];
} HdBackend;

// Private backing-store descriptor for one emulated SCSI hard disk.
// The disk command handler sees only HdBackend operations, not file/flash details.
typedef struct {
    HdBackend backend;
} HdPriv;

static HdPriv *gHdPrivs[SCSI_TARGET_COUNT] = {};
static SCSIDevice *gHdDevices[SCSI_TARGET_COUNT] = {};
static portMUX_TYPE gHdMountedPathMux = portMUX_INITIALIZER_UNLOCKED;

// Resolve the configured HD name into an actual filesystem path.
// Relative names are rooted under /sd/ so setting.txt can stay compact.
static const char *resolveHdPath(const char *file, char *buf, size_t bufSize) {
    if (!file || file[0] == '\0') {
        return NULL;
    }
    if (file[0] == '/') {
        return file;
    }
    snprintf(buf, bufSize, "/sd/%s", file);
    return buf;
}

static void hdBackendSetMountedPath(HdBackend *backend, const char *path) {
    snprintf(backend->mountedPath, sizeof(backend->mountedPath), "%s", path ? path : "");
}

static const uint8_t inq_resp[95]={
    0, //HD
    0, //0x80 if removable
    0x49, //Obsolete SCSI standard 1 all the way
    0, //response version etc
    31, //extra data
    0,0, //reserved
    0, //features
    'A','P','P','L','E',' ',' ',' ', //vendor id
    '2','0','S','C',' ',' ',' ',' ', //prod id
    '1','.','0',' ',' ',' ',' ',' ', //prod rev lvl
};

static bool hdBackendOpenFile(HdBackend *backend, const char *path) {
    if (!storageBackendOpenFile(&backend->fileBackend, path, true)) {
        return false;
    }
    backend->size = (int)backend->fileBackend.size;
    hdBackendSetMountedPath(backend, path);
    if (kHdTrace) {
        consoleLogPrintf("HD: Using SD card %s (%d bytes, %s)\n",
                         path, backend->size,
                         backend->fileBackend.readOnly ? "read-only" : "read/write");
    }
    return true;
}

static bool hdBackendOpenFlash(HdBackend *backend) {
    backend->part=esp_partition_find_first((esp_partition_type_t)0x40,
                                           (esp_partition_subtype_t)0x02,
                                           NULL);
    if (backend->part==0) {
        consoleLogPrintf("HD: No SD card image and no flash partition!\n");
        return false;
    }
    backend->size=backend->part->size;
    if (kHdTrace) {
        consoleLogPrintf("HD: Using flash partition (%d bytes)\n", backend->size);
    }
    return true;
}

static void hdBackendClose(HdBackend *backend) {
    storageBackendClose(&backend->fileBackend);
    memset(backend, 0, sizeof(*backend));
}

static bool hdBackendHasMedia(const HdBackend *backend) {
    return backend->fileBackend.file || backend->part;
}

static const char *hdBackendMountedPath(const HdBackend *backend) {
    return backend ? backend->mountedPath : "";
}

// Update one 512-byte logical sector inside the flash-backed HD image.
// Steps:
// 1. load the owning 4 KB erase block,
// 2. replace just the target sector,
// 3. erase and rewrite the whole block.
static bool hdBackendWriteFlashSector(HdBackend *backend, unsigned int lba, const uint8_t *data) {
    uint8_t *secdat=(uint8_t*)malloc(4096);
    if (!secdat) {
        consoleLogPrintf("HD: cannot allocate flash erase buffer\n");
        return false;
    }
    unsigned int lbaStart=lba&(~7);
    unsigned int lbaOff=lba&7;
    bool ok = esp_partition_read(backend->part, lbaStart*512, secdat, 4096)==ESP_OK &&
              esp_partition_erase_range(backend->part, lbaStart*512, 4096)==ESP_OK;
    if (ok) {
        for (int i=0; i<512; i++) secdat[lbaOff*512+i]=data[i];
        ok = esp_partition_write(backend->part, lbaStart*512, secdat, 4096)==ESP_OK;
    }
    free(secdat);
    return ok;
}

static bool hdBackendRead(HdBackend *backend, unsigned int lba, uint8_t *destination, size_t byteCount) {
    if (backend->fileBackend.file) {
        return storageBackendRead(&backend->fileBackend,
                                  (size_t)lba * 512U,
                                  destination,
                                  byteCount);
    }
    return backend->part &&
           esp_partition_read(backend->part, lba * 512, destination, byteCount) == ESP_OK;
}

static bool hdBackendWrite(HdBackend *backend, unsigned int lba, const uint8_t *source, unsigned int blocks) {
    const size_t byteCount = (size_t)blocks * 512U;
    if (backend->fileBackend.file) {
        return storageBackendWrite(&backend->fileBackend, (size_t)lba * 512U, source, byteCount) &&
               storageBackendFlush(&backend->fileBackend);
    }

    if (!backend->part) {
        return false;
    }
    while (blocks) {
        if (!hdBackendWriteFlashSector(backend, lba, source)) {
            return false;
        }
        lba++;
        source += 512;
        blocks--;
    }
    return true;
}

// Hide the storage medium from the disk command handler. NCR remains the SCSI
// bus/controller model; this code is only the selected target device.
static bool hdRead(HdPriv *hd, unsigned int lba, uint8_t *destination, size_t byteCount) {
    return hdBackendRead(&hd->backend, lba, destination, byteCount);
}

static bool hdWrite(HdPriv *hd, unsigned int lba, const uint8_t *source, unsigned int blocks) {
    return hdBackendWrite(&hd->backend, lba, source, blocks);
}

// Execute the subset of SCSI disk commands the Mac ROM uses.
// Steps:
// 1. decode read, write, inquiry, or capacity requests,
// 2. serve them from an SD file or the flash fallback,
// 3. return SCSI status bytes through the shared transfer buffer.
static int hdScsiCmd(SCSITransferData *data, unsigned int cmd, unsigned int len, unsigned int lba, void *arg) {
    int ret=0;
    HdPriv *hd=(HdPriv*)arg;
    const size_t byteCount = (size_t)len * 512U;

    if (cmd==0x8 || cmd==0x28) { //read
        if (byteCount > SCSI_DATA_BUFFER_SIZE) {
            consoleLogPrintf("HD: Reject oversized read %u bytes from LBA %d.\n",
                             (unsigned)byteCount,
                             lba);
            data->cmd[0] = 2; // CHECK CONDITION
            data->msg[0] = 0;
            return 0;
        }
        if (kHdTrace && appConsoleOutEnabled()) {
            consoleLogPrintf("HD: Read %u bytes from LBA %d.\n", (unsigned)byteCount, lba);
        }
        // Backend failures become SCSI CHECK CONDITION without involving NCR.
        if (!hdRead(hd, lba, data->data, byteCount)) {
            data->cmd[0] = 2; // CHECK CONDITION
            data->msg[0] = 0;
            return 0;
        }
        ret=(int)byteCount;
    } else if (cmd==0x0A || cmd==0x2A) { //write
        if (byteCount > SCSI_DATA_BUFFER_SIZE) {
            consoleLogPrintf("HD: Reject oversized write %u bytes to LBA %d.\n",
                             (unsigned)byteCount,
                             lba);
            data->cmd[0] = 2; // CHECK CONDITION
            data->msg[0] = 0;
            return 0;
        }
        if (kHdTrace && appConsoleOutEnabled()) {
            consoleLogPrintf("HD: Write %u bytes to LBA %d.\n", (unsigned)byteCount, lba);
        }
        // Backend failures become SCSI CHECK CONDITION without involving NCR.
        if (!hdWrite(hd, lba, data->data, len)) {
            data->cmd[0] = 2; // CHECK CONDITION
            data->msg[0] = 0;
            return 0;
        }
        ret=0;
    } else if (cmd==0x12) { //inquiry
        if (kHdTrace && appConsoleOutEnabled()) {
            consoleLogPrintf("HD: Inquiry\n");
        }
        memcpy(data->data, inq_resp, sizeof(inq_resp));
        return 95;
    } else if (cmd==0x25) { //read capacity
        int lbacnt=hd->backend.size/512;
        data->data[0]=(lbacnt>>24);
        data->data[1]=(lbacnt>>16);
        data->data[2]=(lbacnt>>8);
        data->data[3]=(lbacnt>>0);
        data->data[4]=0;
        data->data[5]=0;
        data->data[6]=2; //512
        data->data[7]=0;
        ret=8;
        if (kHdTrace && appConsoleOutEnabled()) {
            consoleLogPrintf("HD: Read capacity (%d)\n", lbacnt);
        }
    } else {
        consoleLogPrintf("********** hdScsiCmd: unrecognized command %x\n", cmd);
    }
    data->cmd[0]=0; //status
    data->msg[0]=0;
    return ret;
}

// Create one emulated SCSI hard disk device.
// Steps:
// 1. try the configured SD image,
// 2. fall back through legacy SD filenames,
// 3. use the flash partition when no SD image is available.
SCSIDevice *hdCreate(const char *file) {
    SCSIDevice *ret=(SCSIDevice*)malloc(sizeof(SCSIDevice));
    if (!ret) {
        consoleLogPrintf("HD: cannot allocate SCSI device\n");
        return nullptr;
    }
    memset(ret, 0, sizeof(SCSIDevice));
    HdPriv *hd=(HdPriv*)malloc(sizeof(HdPriv));
    if (!hd) {
        consoleLogPrintf("HD: cannot allocate private state\n");
        free(ret);
        return nullptr;
    }
    memset(hd, 0, sizeof(HdPriv));

    // Try the configured SD image first, then legacy filenames for compatibility.
    if (sdcardMounted()) {
        char configuredPath[160];
        const char *configured = resolveHdPath(file, configuredPath, sizeof(configuredPath));
        const char *legacyNames[] = { "/sd/hd.img", "/sd/hd.hd", "/sd/hd.dsk", NULL };

        if (configured) {
            hdBackendOpenFile(&hd->backend, configured);
        }

        for (int i = 0; legacyNames[i] && !hdBackendHasMedia(&hd->backend); i++) {
            if (configured && strcmp(legacyNames[i], configured) == 0) {
                continue;
            }
            hdBackendOpenFile(&hd->backend, legacyNames[i]);
        }
    }

    // Fall back to the legacy flash partition when no SD image is available.
    if (!hdBackendHasMedia(&hd->backend)) {
        hdBackendOpenFlash(&hd->backend);
    }

    ret->arg=hd;
    ret->scsiCmd=hdScsiCmd;
    for (int i = 0; i < SCSI_TARGET_COUNT; ++i) {
        if (!gHdPrivs[i]) {
            gHdPrivs[i] = hd;
            gHdDevices[i] = ret;
            break;
        }
    }
    return ret;
}

void hdGetMountedPath(int target, char *destination, size_t destinationSize) {
    if (target < 0 || target >= SCSI_TARGET_COUNT || !destination || destinationSize == 0) {
        return;
    }
    taskENTER_CRITICAL(&gHdMountedPathMux);
    snprintf(destination,
             destinationSize,
             "%s",
             gHdPrivs[target] ? hdBackendMountedPath(&gHdPrivs[target]->backend) : "");
    taskEXIT_CRITICAL(&gHdMountedPathMux);
}

void hdShutdownAll(void) {
    for (int i = 0; i < SCSI_TARGET_COUNT; ++i) {
        HdPriv *hd = gHdPrivs[i];
        if (hd) {
            hdBackendClose(&hd->backend);
            free(hd);
            gHdPrivs[i] = nullptr;
        }
        if (gHdDevices[i]) {
            free(gHdDevices[i]);
            gHdDevices[i] = nullptr;
        }
    }
}
