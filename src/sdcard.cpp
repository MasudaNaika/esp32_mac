/*
 * MicroSD card support using SDMMC 1-bit native mode.
 * Pins shared with LCD SPI init (GPIO1, GPIO2) — LCD SPI bus must be freed first.
 * GPIO42 is SD D0 (MISO). EXIO4 (DAT3) not needed in 1-bit mode.
 */
#include "sdcard.h"
#include "TCA9554PWR.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#define SD_CLK  GPIO_NUM_2
#define SD_CMD  GPIO_NUM_1
#define SD_D0   GPIO_NUM_42

static bool mounted = false;
static sdmmc_card_t *mountedCard = NULL;

// Mount the shared `/sd` volume using SDMMC 1-bit mode.
// Steps:
// 1. prepare the shared pins,
// 2. configure the host and slot,
// 3. mount the FAT filesystem and remember the card handle.
bool sdcardInit() {
    if (mounted) {
        return true;
    }

    // Pull DAT3/CS high via IO expander (inactive state)
    Set_EXIO(EXIO_PIN4, High);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.unaligned_multi_block_rw_max_chunk_size = 8;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_CLK;
    slot_config.cmd = SD_CMD;
    slot_config.d0 = SD_D0;
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot_config, &mount_config, &mountedCard);
    if (ret != ESP_OK) {
        printf("SD card: not available (%s)\n", esp_err_to_name(ret));
        mountedCard = NULL;
        return false;
    }

    printf("SD card mounted:\n");
    sdmmc_card_print_info(stdout, mountedCard);
    mounted = true;
    return true;
}

// Unmount the shared `/sd` volume and clear the cached card state.
void sdcardDeinit() {
    if (!mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount("/sd", mountedCard);
    mountedCard = NULL;
    mounted = false;
    printf("SD card unmounted\n");
}

// Report whether the shared SD volume is currently mounted.
bool sdcardMounted() {
    return mounted;
}
