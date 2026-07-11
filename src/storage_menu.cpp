#include "storage_menu.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_settings.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "pv_fdd.h"
#include "storage_usb.h"
#include "tme/tmeconfig.h"

extern "C" {
#include "tme/disp.h"
}

namespace {

constexpr size_t kScsiTargets = SCSI_TARGET_COUNT;
// Keep directory data in PSRAM. 256 entries use 32 KiB per FD/HD list.
constexpr size_t kMaxImages = 256;
constexpr size_t kMaxName = 128;
constexpr size_t kVisibleRows = 13;
constexpr uint8_t kMenuLastContentRow = 18;
constexpr uint8_t kListFirstRow = 5;

struct ImageList {
    char names[kMaxImages][kMaxName];
    size_t count;
};

int compareNames(const void *a, const void *b) {
    return strcasecmp(static_cast<const char *>(a), static_cast<const char *>(b));
}

void readImages(const char *directory, ImageList *list) {
    memset(list, 0, sizeof(*list));
    DIR *dir = opendir(directory);
    if (!dir) return;
    while (dirent *entry = readdir(dir)) {
        if (entry->d_name[0] == '.' || entry->d_type == DT_DIR ||
            strlen(entry->d_name) >= kMaxName || list->count == kMaxImages) {
            continue;
        }
        snprintf(list->names[list->count++], kMaxName, "%s", entry->d_name);
    }
    closedir(dir);
    qsort(list->names, list->count, sizeof(list->names[0]), compareNames);
}

const char *shortName(const char *path) {
    if (!path || !path[0]) return "none";
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void writeFrame(const char *title) {
    dispConsoleWriteRow(0, false, "+-----------------------------------------------+");
    dispConsoleWriteRow(1, false, "| %-45.45s |", title);
    dispConsoleWriteRow(2, false, "+-----------------------------------------------+");
}

void writeOption(uint8_t row, bool selected, const char *text) {
    dispConsoleWriteRow(row, false, "| %c %-43.43s |", selected ? '>' : ' ', text);
}

void writeFooter(uint8_t row) {
    dispConsoleWriteRow(row, false, "+-----------------------------------------------+");
    dispConsoleSetStatus("Short: next  Long: select");
}

void writePromptStatus() {
    dispConsoleSetStatus("Short: next  Long: select");
}

void clearMenuRows(uint8_t firstRow) {
    for (uint8_t row = firstRow; row <= kMenuLastContentRow; ++row) {
        dispConsoleWriteRow(row, false, "");
    }
}

size_t listFirstForSelection(size_t selected, size_t count) {
    size_t first = 0;
    if (count > kVisibleRows && selected >= kVisibleRows / 2) {
        first = selected - kVisibleRows / 2;
        if (first + kVisibleRows > count) first = count - kVisibleRows;
    }
    return first;
}

const char *imageChoiceName(size_t index, const ImageList &images) {
    if (index < images.count) return images.names[index];
    return index == images.count ? "none" : "back";
}

void writeImageOptionRow(uint8_t row, size_t index, size_t selected,
                         const ImageList &images) {
    writeOption(row, index == selected, imageChoiceName(index, images));
}

void renderImageMenu(const char *title, const char *mounted,
                     const ImageList &images, size_t selected) {
    const size_t count = images.count + 2;
    const size_t first = listFirstForSelection(selected, count);
    size_t last = first + kVisibleRows;
    if (last > count) last = count;

    const size_t fileFirst = first < images.count ? first : images.count;
    const size_t fileLast = last < images.count ? last : images.count;
    char windowTitle[64];
    if (images.count == 0) {
        snprintf(windowTitle, sizeof(windowTitle), "%s  0/0", title);
    } else {
        snprintf(windowTitle, sizeof(windowTitle), "%s  %u-%u/%u", title,
                 static_cast<unsigned>(fileFirst + 1),
                 static_cast<unsigned>(fileLast), static_cast<unsigned>(images.count));
    }
    writeFrame(windowTitle);
    dispConsoleWriteRow(3, false, "| mounted: %-36.36s |", shortName(mounted));
    dispConsoleWriteRow(4, false, "+-----------------------------------------------+");
    uint8_t row = kListFirstRow;
    for (size_t i = first; i < last; ++i, ++row) {
        writeImageOptionRow(row, i, selected, images);
    }
    writeFooter(row++);
    clearMenuRows(row);
}

void renderFdMenu(size_t selected, const char fddNames[PV_FDD_DRIVE_COUNT][kMaxName],
                  const char *const mountedFdds[PV_FDD_DRIVE_COUNT]) {
    writeFrame("Setting / FD");
    char line[128];
    uint8_t row = 3;
    for (size_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive, ++row) {
        const char *mounted = mountedFdds ? mountedFdds[drive] : fddNames[drive];
        snprintf(line, sizeof(line), "fd%u  mounted: %s",
                 static_cast<unsigned>(drive), shortName(mounted));
        writeOption(row, selected == drive, line);
    }
    writeOption(row++, selected == PV_FDD_DRIVE_COUNT, "back");
    writeFooter(row++);
    clearMenuRows(row);
}

void writeFdSelectionRow(size_t index, bool selected,
                         const char fddNames[PV_FDD_DRIVE_COUNT][kMaxName],
                         const char *const mountedFdds[PV_FDD_DRIVE_COUNT]) {
    char line[128];
    if (index < PV_FDD_DRIVE_COUNT) {
        const char *mounted = mountedFdds ? mountedFdds[index] : fddNames[index];
        snprintf(line, sizeof(line), "fd%u  mounted: %s",
                 static_cast<unsigned>(index), shortName(mounted));
        writeOption(static_cast<uint8_t>(3 + index), selected, line);
    } else {
        writeOption(static_cast<uint8_t>(3 + PV_FDD_DRIVE_COUNT), selected, "back");
    }
}

void renderHdMenu(size_t selected, const char hdNames[kScsiTargets][kMaxName],
                  const char *const mountedHds[kScsiTargets]) {
    writeFrame("Setting / HDD");
    uint8_t row = 3;
    for (size_t id = 0; id < kScsiTargets; ++id, ++row) {
        char line[128];
        const char *mounted = mountedHds ? mountedHds[id] : hdNames[id];
        snprintf(line, sizeof(line), "hd%u  mounted: %s",
                 static_cast<unsigned>(id), shortName(mounted));
        writeOption(row, selected == id, line);
    }
    writeOption(row++, selected == kScsiTargets, "restart emu");
    writeOption(row++, selected == kScsiTargets + 1, "back");
    writeFooter(row++);
    clearMenuRows(row);
}

void writeHdSelectionRow(size_t index, bool selected,
                         const char hdNames[kScsiTargets][kMaxName],
                         const char *const mountedHds[kScsiTargets]) {
    char line[128];
    if (index < kScsiTargets) {
        const char *mounted = mountedHds ? mountedHds[index] : hdNames[index];
        snprintf(line, sizeof(line), "hd%u  mounted: %s",
                 static_cast<unsigned>(index), shortName(mounted));
        writeOption(static_cast<uint8_t>(3 + index), selected, line);
    } else if (index == kScsiTargets) {
        writeOption(static_cast<uint8_t>(3 + kScsiTargets), selected, "restart emu");
    } else {
        writeOption(static_cast<uint8_t>(4 + kScsiTargets), selected, "back");
    }
}

void settingsOptionText(size_t index, char *line, size_t lineSize) {
    if (index == 0) {
        snprintf(line, lineSize, "fd");
    } else if (index == 1) {
        snprintf(line, lineSize, "hdd");
    } else if (index == 2) {
        snprintf(line, lineSize, "turbo  %s", appTurboEnabled() ? "on" : "off");
    } else if (index == 3) {
        snprintf(line, lineSize, "boot info  %s", appBootInfoEnabled() ? "on" : "off");
    } else if (index == 4) {
        snprintf(line, lineSize, "show QR");
    } else if (index == 5) {
        snprintf(line, lineSize, "usb storage");
    } else {
        snprintf(line, lineSize, "to emu");
    }
}

void writeSettingsOptionRow(size_t index, bool selected) {
    char line[64];
    settingsOptionText(index, line, sizeof(line));
    writeOption(static_cast<uint8_t>(3 + index), selected, line);
}

void renderSettingsMenu(size_t selected) {
    writeFrame("Setting");
    for (size_t i = 0; i < 7; ++i) {
        writeSettingsOptionRow(i, selected == i);
    }
    writeFooter(10);
    clearMenuRows(11);
}

void showQrScreen() {
    char url[64];
    if (!macHttpGetActiveUrl(url, sizeof(url))) {
        dispConsoleSetStatus("QR unavailable until WiFi starts");
        vTaskDelay(pdMS_TO_TICKS(1000));
        writePromptStatus();
        return;
    }
    if (!dispConsoleShowQrCode("Scan to open Web UI", url, "Short/Long: back")) {
        dispConsoleSetStatus("QR generation failed");
        vTaskDelay(pdMS_TO_TICKS(1000));
        writePromptStatus();
        return;
    }
    dispWaitButtonEvent(UINT32_MAX);
    dispConsoleHideQrCode();
}

bool selectImage(const char *title, const char *folder, const char *settingKey,
                 const ImageList &images, char *value, size_t valueSize,
                 const char *mounted, bool *changed) {
    // File entries are followed by the two fixed actions requested by the UI.
    const size_t backIndex = images.count + 1;
    const size_t count = images.count + 2;
    size_t selected = images.count;
    if (value && value[0]) {
        for (size_t i = 0; i < images.count; ++i) {
            char path[kMaxName];
            snprintf(path, sizeof(path), "%s/%s", folder, images.names[i]);
            if (strcasecmp(value, path) == 0) {
                selected = i;
                break;
            }
        }
    }

    dispConsoleClear();
    renderImageMenu(title, mounted, images, selected);
    while (true) {
        int event = dispWaitButtonEvent(UINT32_MAX);
        if (event == 1) {
            const size_t oldSelected = selected;
            const size_t oldFirst = listFirstForSelection(selected, count);
            selected = (selected + 1) % count;
            const size_t newFirst = listFirstForSelection(selected, count);
            if (oldFirst == newFirst) {
                writeImageOptionRow(static_cast<uint8_t>(kListFirstRow + oldSelected - oldFirst),
                                    oldSelected, selected, images);
                writeImageOptionRow(static_cast<uint8_t>(kListFirstRow + selected - newFirst),
                                    selected, selected, images);
            } else {
                renderImageMenu(title, mounted, images, selected);
            }
            continue;
        }
        if (event != 2) continue;
        if (selected == backIndex) return false;

        char selectedPath[kMaxName] = {};
        if (selected < images.count) {
            snprintf(selectedPath, sizeof(selectedPath), "%s/%s", folder,
                     images.names[selected]);
        }
        if (strcasecmp(value, selectedPath) != 0) {
            snprintf(value, valueSize, "%s", selectedPath);
            if (appSaveStorageSelection(settingKey, value)) *changed = true;
        }
        return true;
    }
}

void runFdMenu(char fddNames[PV_FDD_DRIVE_COUNT][kMaxName],
               const char *const mountedFdds[PV_FDD_DRIVE_COUNT],
               const ImageList &floppies,
               bool *changed) {
    size_t fdSelected = 0;
    dispConsoleClear();
    renderFdMenu(fdSelected, fddNames, mountedFdds);
    while (true) {
        int event = dispWaitButtonEvent(UINT32_MAX);
        if (event == 1) {
            const size_t oldSelected = fdSelected;
            fdSelected = (fdSelected + 1) % (PV_FDD_DRIVE_COUNT + 1);
            writeFdSelectionRow(oldSelected, false, fddNames, mountedFdds);
            writeFdSelectionRow(fdSelected, true, fddNames, mountedFdds);
            continue;
        }
        if (event != 2) continue;
        if (fdSelected == PV_FDD_DRIVE_COUNT) return;
        if (fdSelected < PV_FDD_DRIVE_COUNT) {
            if (mountedFdds && mountedFdds[fdSelected] &&
                mountedFdds[fdSelected][0] != '\0') {
                dispConsoleSetStatus("Eject FD%u in Mac first",
                                     static_cast<unsigned>(fdSelected));
                vTaskDelay(pdMS_TO_TICKS(1000));
                writePromptStatus();
                continue;
            }
            char key[4];
            char title[16];
            snprintf(key, sizeof(key), "fd%u", static_cast<unsigned>(fdSelected));
            snprintf(title, sizeof(title), "FD%u file", static_cast<unsigned>(fdSelected));
            const char *mounted = mountedFdds ? mountedFdds[fdSelected] : fddNames[fdSelected];
            selectImage(title, "/sd/fd", key, floppies,
                        fddNames[fdSelected], kMaxName, mounted, changed);
            renderFdMenu(fdSelected, fddNames, mountedFdds);
            continue;
        }
    }
}

StorageMenuResult runHdMenu(char hdNames[kScsiTargets][kMaxName],
                            const char *const mountedHds[kScsiTargets],
                            const ImageList &hardDisks,
                            bool *changed) {
    size_t hdSelected = 0;
    dispConsoleClear();
    renderHdMenu(hdSelected, hdNames, mountedHds);
    while (true) {
        int event = dispWaitButtonEvent(UINT32_MAX);
        if (event == 1) {
            const size_t oldSelected = hdSelected;
            hdSelected = (hdSelected + 1) % (kScsiTargets + 2);
            writeHdSelectionRow(oldSelected, false, hdNames, mountedHds);
            writeHdSelectionRow(hdSelected, true, hdNames, mountedHds);
            continue;
        }
        if (event != 2) continue;
        if (hdSelected == kScsiTargets) return StorageMenuResult::RestartEmu;
        if (hdSelected == kScsiTargets + 1) return StorageMenuResult::ReturnToConsole;

        char key[4];
        char title[16];
        snprintf(key, sizeof(key), "hd%u", static_cast<unsigned>(hdSelected));
        snprintf(title, sizeof(title), "HD%u file", static_cast<unsigned>(hdSelected));
        const char *mounted = mountedHds ? mountedHds[hdSelected] : hdNames[hdSelected];
        selectImage(title, "/sd/hd", key, hardDisks,
                    hdNames[hdSelected], kMaxName, mounted, changed);
        renderHdMenu(hdSelected, hdNames, mountedHds);
    }
}

StorageMenuResult runSettings(char fddNames[PV_FDD_DRIVE_COUNT][kMaxName],
                              char hdNames[kScsiTargets][kMaxName],
                              const char *const mountedFdds[PV_FDD_DRIVE_COUNT],
                              const char *const mountedHds[kScsiTargets],
                              const ImageList &floppies,
                              const ImageList &hardDisks,
                              bool *changed) {
    size_t selected = 0;
    dispConsoleClear();
    renderSettingsMenu(selected);
    while (true) {
        int event = dispWaitButtonEvent(UINT32_MAX);
        if (event == 1) {
            const size_t oldSelected = selected;
            selected = (selected + 1) % 7;
            writeSettingsOptionRow(oldSelected, false);
            writeSettingsOptionRow(selected, true);
            continue;
        }
        if (event != 2) continue;
        if (selected == 0) {
            runFdMenu(fddNames, mountedFdds, floppies, changed);
            renderSettingsMenu(selected);
        } else if (selected == 1) {
            StorageMenuResult result = runHdMenu(hdNames, mountedHds, hardDisks, changed);
            if (result == StorageMenuResult::RestartEmu) return result;
            renderSettingsMenu(selected);
        } else if (selected == 2) {
            bool enabled = !appTurboEnabled();
            appSetTurboEnabled(enabled);
            if (appSaveBoolSetting("turbo", enabled)) {
                *changed = true;
            }
            dispConsoleSetStatus("turbo %s", enabled ? "on" : "off");
            writeSettingsOptionRow(selected, true);
            vTaskDelay(pdMS_TO_TICKS(500));
            writePromptStatus();
        } else if (selected == 3) {
            bool enabled = !appBootInfoEnabled();
            appSetBootInfoEnabled(enabled);
            if (appSaveBoolSetting("boot_info", enabled)) {
                *changed = true;
            }
            dispConsoleSetStatus("boot info %s", enabled ? "on" : "off");
            writeSettingsOptionRow(selected, true);
            vTaskDelay(pdMS_TO_TICKS(500));
            writePromptStatus();
        } else if (selected == 4) {
            showQrScreen();
            renderSettingsMenu(selected);
        } else if (selected == 5) {
            return StorageMenuResult::UsbStorage;
        } else {
            return StorageMenuResult::ReturnToEmu;
        }
    }
}

StorageMenuResult runMenus(char fddNames[PV_FDD_DRIVE_COUNT][kMaxName],
                           char hdNames[kScsiTargets][kMaxName],
                           const char *const mountedFdds[PV_FDD_DRIVE_COUNT],
                           const char *const mountedHds[kScsiTargets],
                           const ImageList &floppies,
                           const ImageList &hardDisks,
                           bool *changed) {
    return runSettings(fddNames, hdNames, mountedFdds, mountedHds,
                       floppies, hardDisks, changed);
}

char sMountedFdds[PV_FDD_DRIVE_COUNT][kMaxName] = {};
char sMountedHds[kScsiTargets][kMaxName] = {};

void resetConsoleAfterMenu() {
    dispConsoleClear();
    dispConsolePrintf("Console view\n");
}

void runtimeMenuTask(void *param) {
    (void)param;
    while (true) {
        if (!dispTakeStorageMenuRequest()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        AppSettings current;
        loadSettingsFromSd(&current);
        appSetTurboEnabled(current.turbo);
        appSetBootInfoEnabled(current.bootInfo);
        const char *mountedFddPtrs[PV_FDD_DRIVE_COUNT];
        for (size_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
            pvFddGetMountedName(drive, sMountedFdds[drive], sizeof(sMountedFdds[drive]));
            mountedFddPtrs[drive] = sMountedFdds[drive];
        }
        const char *mountedPtrs[kScsiTargets];
        for (size_t id = 0; id < kScsiTargets; ++id) {
            mountedPtrs[id] = sMountedHds[id];
        }
        dispSetButtonMenuMode(true);
        StorageMenuResult result = storageMenuRun(current.fd, current.hd,
                                                  mountedFddPtrs, mountedPtrs);
        if (result != StorageMenuResult::RestartEmu) {
            for (size_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
                if (strcmp(current.fd[drive], sMountedFdds[drive]) != 0 &&
                    !pvFddRequestImage(drive, current.fd[drive])) {
                    dispConsoleSetStatus("FD%u request failed",
                                         static_cast<unsigned>(drive));
                }
            }
        }
        if (result == StorageMenuResult::RestartEmu) {
            dispConsoleSetStatus("Restarting emulator...");
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        if (result == StorageMenuResult::UsbStorage) {
            dispConsoleSetStatus("Starting USB storage...");
            storageUsbStartMassStorageMode();
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        resetConsoleAfterMenu();
        dispSetButtonMenuMode(false);
        if (result == StorageMenuResult::ReturnToConsole) {
            dispShowConsole();
        } else {
            dispShowEmu();
        }
    }
}

}  // namespace

StorageMenuResult storageMenuRun(char fddNames[PV_FDD_DRIVE_COUNT][128],
                                 char hdNames[SCSI_TARGET_COUNT][128],
                                 const char *const mountedFdds[PV_FDD_DRIVE_COUNT],
                                 const char *const mountedHds[SCSI_TARGET_COUNT]) {
    dispConsoleHideQrCode();
    ImageList *lists = static_cast<ImageList *>(tme_psram_aligned_calloc(
        2, sizeof(ImageList), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!lists) {
        dispConsolePrintf("Disk menu: PSRAM allocation failed\n");
        return StorageMenuResult::ReturnToConsole;
    }
    readImages("/sd/fd", &lists[0]);
    readImages("/sd/hd", &lists[1]);
    bool changed = false;
    dispSetAutoStatusEnabled(false);
    StorageMenuResult result = runMenus(fddNames, hdNames, mountedFdds,
                                        mountedHds, lists[0], lists[1], &changed);
    dispSetAutoStatusEnabled(true);
    heap_caps_free(lists);
    (void)changed;
    return result;
}

void storageMenuStartRuntime(const char mountedFdds[PV_FDD_DRIVE_COUNT][128],
                             const char mountedHds[SCSI_TARGET_COUNT][128]) {
    for (size_t drive = 0; drive < PV_FDD_DRIVE_COUNT; ++drive) {
        snprintf(sMountedFdds[drive], sizeof(sMountedFdds[drive]), "%s",
                 mountedFdds ? mountedFdds[drive] : "");
    }
    for (size_t id = 0; id < kScsiTargets; ++id) {
        snprintf(sMountedHds[id], sizeof(sMountedHds[id]), "%s", mountedHds[id]);
    }
    xTaskCreatePinnedToCore(runtimeMenuTask, "disk_menu", 4096, NULL, 2, NULL, 0);
}
