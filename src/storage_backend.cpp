#include "storage_backend.h"

#include <limits.h>
#include <string.h>

bool storageBackendOpenFile(StorageBackend *backend,
                            const char *path,
                            bool writable) {
    if (!backend || !path || !path[0]) {
        return false;
    }

    memset(backend, 0, sizeof(*backend));

    FILE *file = nullptr;
    bool readOnly = false;
    // Prefer read/write media, but keep booting from read-only SD images.
    if (writable) {
        file = fopen(path, "r+b");
    }
    if (!file) {
        file = fopen(path, "rb");
        readOnly = file != nullptr;
    }
    if (!file) {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }

    backend->file = file;
    backend->size = (size_t)size;
    backend->readOnly = readOnly || !writable;
    snprintf(backend->path, sizeof(backend->path), "%s", path);
    return true;
}

bool storageBackendRead(StorageBackend *backend,
                        size_t offset,
                        void *destination,
                        size_t length) {
    if (!backend || !backend->file || !destination || offset > (size_t)LONG_MAX) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (fseek(backend->file, (long)offset, SEEK_SET) != 0) {
        return false;
    }
    // A short read is a media/backend failure for the callers, not success.
    return fread(destination, 1, length, backend->file) == length;
}

bool storageBackendWrite(StorageBackend *backend,
                         size_t offset,
                         const void *source,
                         size_t length) {
    if (!backend || !backend->file || !source || backend->readOnly ||
        offset > (size_t)LONG_MAX) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (fseek(backend->file, (long)offset, SEEK_SET) != 0) {
        return false;
    }
    // A short write is reported so device code can map it to its own error.
    return fwrite(source, 1, length, backend->file) == length;
}

bool storageBackendFlush(StorageBackend *backend) {
    if (!backend || !backend->file) {
        return false;
    }
    return fflush(backend->file) == 0;
}

void storageBackendClose(StorageBackend *backend) {
    if (!backend || !backend->file) {
        return;
    }
    storageBackendFlush(backend);
    fclose(backend->file);
    memset(backend, 0, sizeof(*backend));
}
