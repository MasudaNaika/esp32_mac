#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

// Thin byte-addressed file backend shared by the SCSI HD and PV floppy code.
// It intentionally knows nothing about SCSI commands, floppy geometry, or Mac
// Device Manager state; callers keep those semantics in their device layers.
typedef struct {
    FILE *file;
    size_t size;
    bool readOnly;
    char path[160];
} StorageBackend;

bool storageBackendOpenFile(StorageBackend *backend,
                            const char *path,
                            bool writable);
bool storageBackendRead(StorageBackend *backend,
                        size_t offset,
                        void *destination,
                        size_t length);
bool storageBackendWrite(StorageBackend *backend,
                         size_t offset,
                         const void *source,
                         size_t length);
bool storageBackendFlush(StorageBackend *backend);
void storageBackendClose(StorageBackend *backend);
