#include "storage_usb.h"

#include <stdlib.h>
#include <stdio.h>

#include "TCA9554PWR.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tme/tmeconfig.h"

extern "C" {
#include "tme/disp.h"
#include "tme/emu.h"
}

#define SD_CLK  GPIO_NUM_2
#define SD_CMD  GPIO_NUM_1
#define SD_D0   GPIO_NUM_42

namespace {

constexpr int kStopWaitMs = 5000;
constexpr int kRestartDelayMs = 500;
constexpr size_t kCdcRxBufferSlack = 16;
constexpr size_t kCdcRxBufferSize = CONFIG_TINYUSB_CDC_RX_BUFSIZE + kCdcRxBufferSlack;

static const char *TAG = "storage_usb";
static bool gStorageModeActive = false;
static bool gUsbMountedOnce = false;
static bool gRestartScheduled = false;
static tinyusb_msc_storage_handle_t gStorageHandle = nullptr;
static sdmmc_card_t *gUsbCard = nullptr;
static uint8_t *gCdcRxBuffer = nullptr;

void restartTask(void *param) {
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(kRestartDelayMs));
    esp_restart();
}

void scheduleRestart(void) {
    if (gRestartScheduled) {
        return;
    }
    gRestartScheduled = true;
    xTaskCreatePinnedToCore(restartTask, "usb_reboot", 2048, nullptr, 5, nullptr, 0);
}

esp_err_t initSdCardForUsb(sdmmc_card_t **card) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.unaligned_multi_block_rw_max_chunk_size = 8;

    sdmmc_slot_config_t slotConfig = SDMMC_SLOT_CONFIG_DEFAULT();
    slotConfig.clk = SD_CLK;
    slotConfig.cmd = SD_CMD;
    slotConfig.d0 = SD_D0;
    slotConfig.width = 1;
    slotConfig.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    Set_EXIO(EXIO_PIN4, High);

    sdmmc_card_t *sdCard = static_cast<sdmmc_card_t *>(calloc(1, sizeof(sdmmc_card_t)));
    if (!sdCard) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = host.init();
    if (err != ESP_OK) {
        free(sdCard);
        return err;
    }
    err = sdmmc_host_init_slot(host.slot, &slotConfig);
    if (err != ESP_OK) {
        host.deinit();
        free(sdCard);
        return err;
    }
    err = sdmmc_card_init(&host, sdCard);
    if (err != ESP_OK) {
        host.deinit();
        free(sdCard);
        return err;
    }

    sdmmc_card_print_info(stdout, sdCard);
    *card = sdCard;
    return ESP_OK;
}

bool waitForEmulatorStop(void) {
    tmeRequestStop();
    for (int elapsed = 0; elapsed < kStopWaitMs; elapsed += 50) {
        if (tmeEmuStopped()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return tmeEmuStopped();
}

void storageMountChangedCb(tinyusb_msc_storage_handle_t handle,
                           tinyusb_msc_event_t *event,
                           void *arg) {
    (void)handle;
    (void)arg;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE) {
        ESP_LOGI(TAG, "MSC storage mount point: %s",
                 event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP");
        if (gStorageModeActive && gUsbMountedOnce &&
            event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            dispConsoleSetStatus("USB storage ejected, rebooting");
            scheduleRestart();
        }
    } else if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED ||
               event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED) {
        ESP_LOGE(TAG, "MSC storage mount failed or format required");
    }
}

void cdcRxCallback(int itf, cdcacm_event_t *event) {
    (void)event;
    if (!gCdcRxBuffer) {
        return;
    }
    tinyusb_cdcacm_itf_t cdcItf = static_cast<tinyusb_cdcacm_itf_t>(itf);

    size_t rxSize = 0;
    esp_err_t err = tinyusb_cdcacm_read(cdcItf, gCdcRxBuffer,
                                        CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rxSize);
    if (err != ESP_OK || rxSize == 0) {
        return;
    }
    gCdcRxBuffer[rxSize] = '\0';
    ESP_LOGI(TAG, "CDC%u RX %u bytes", (unsigned)itf, (unsigned)rxSize);
    tinyusb_cdcacm_write_queue(cdcItf, gCdcRxBuffer, rxSize);
    tinyusb_cdcacm_write_flush(cdcItf, 0);
}

void cdcLineStateChangedCallback(int itf, cdcacm_event_t *event) {
    ESP_LOGI(TAG, "CDC%u DTR=%d RTS=%d", (unsigned)itf,
             event->line_state_changed_data.dtr,
             event->line_state_changed_data.rts);
}

void usbEventCallback(tinyusb_event_t *event, void *arg) {
    (void)arg;
    if (!gStorageModeActive || !event) {
        return;
    }

    if (event->id == TINYUSB_EVENT_ATTACHED) {
        gUsbMountedOnce = true;
        dispConsoleSetStatus("USB storage connected");
    } else if (event->id == TINYUSB_EVENT_DETACHED && gUsbMountedOnce) {
        dispConsoleSetStatus("USB storage removed, rebooting");
        scheduleRestart();
    }
}

}  // namespace

bool storageUsbStartMassStorageMode(void) {
    if (gStorageModeActive) {
        return true;
    }

    dispSetButtonMenuMode(false);
    dispShowConsole();
    dispConsoleClear();
    dispConsolePrintf("USB Mass Storage mode\n");
    dispConsolePrintf("Stopping emulator...\n");

    if (!waitForEmulatorStop()) {
        dispConsolePrintf("ERROR: emulator stop timeout\n");
        return false;
    }

    dispConsolePrintf("Unmounting app SD filesystem...\n");
    sdcardDeinit();

    esp_err_t err = initSdCardForUsb(&gUsbCard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD init for USB failed: %s", esp_err_to_name(err));
        dispConsolePrintf("ERROR: SD init failed %s\n", esp_err_to_name(err));
        return false;
    }

    tinyusb_msc_storage_config_t storageCfg = {
        .medium = {
            .card = gUsbCard,
        },
        .fat_fs = {
            .base_path = nullptr,
            .config = {
                .format_if_mount_failed = false,
                .max_files = 5,
                .allocation_unit_size = 16 * 1024,
                .disk_status_check_enable = false,
                .use_one_fat = false,
            },
            .do_not_format = true,
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };

    err = tinyusb_msc_new_storage_sdmmc(&storageCfg, &gStorageHandle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MSC storage create failed: %s", esp_err_to_name(err));
        dispConsolePrintf("ERROR: MSC storage failed %s\n", esp_err_to_name(err));
        return false;
    }

    tinyusb_msc_set_storage_callback(storageMountChangedCb, nullptr);

    gStorageModeActive = true;
    gUsbMountedOnce = false;
    gRestartScheduled = false;
    tinyusb_config_t tusbCfg = TINYUSB_DEFAULT_CONFIG();
    tusbCfg.event_cb = usbEventCallback;
    tusbCfg.event_arg = nullptr;
    err = tinyusb_driver_install(&tusbCfg);
    if (err != ESP_OK) {
        gStorageModeActive = false;
        ESP_LOGE(TAG, "TinyUSB install failed: %s", esp_err_to_name(err));
        dispConsolePrintf("ERROR: USB install failed %s\n", esp_err_to_name(err));
        return false;
    }

    gCdcRxBuffer = static_cast<uint8_t *>(tme_psram_aligned_alloc(
        kCdcRxBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!gCdcRxBuffer) {
        gCdcRxBuffer = static_cast<uint8_t *>(heap_caps_malloc(
            kCdcRxBufferSize, MALLOC_CAP_8BIT));
    }
    if (!gCdcRxBuffer) {
        dispConsolePrintf("ERROR: CDC RX buffer alloc failed\n");
        return false;
    }

    tinyusb_config_cdcacm_t acmCfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdcRxCallback,
        .callback_rx_wanted_char = nullptr,
        .callback_line_state_changed = nullptr,
        .callback_line_coding_changed = nullptr,
    };
    err = tinyusb_cdcacm_init(&acmCfg);
    if (err != ESP_OK) {
        gStorageModeActive = false;
        ESP_LOGE(TAG, "CDC ACM init failed: %s", esp_err_to_name(err));
        dispConsolePrintf("ERROR: CDC init failed %s\n", esp_err_to_name(err));
        return false;
    }
    tinyusb_cdcacm_register_callback(TINYUSB_CDC_ACM_0,
                                     CDC_EVENT_LINE_STATE_CHANGED,
                                     cdcLineStateChangedCallback);

    dispConsolePrintf("USB composite storage ready\n");
    dispConsolePrintf("Eject/remove on PC to reboot\n");
    dispConsoleSetStatus("USB composite ready");
    return true;
}
