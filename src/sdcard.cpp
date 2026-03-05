/*
 * MicroSD card support using SDMMC 1-bit native mode.
 * Pins shared with LCD SPI init (GPIO1, GPIO2) — LCD SPI bus must be freed first.
 * GPIO42 is SD D0 (MISO). EXIO4 (DAT3) not needed in 1-bit mode.
 */
#include <Arduino.h>
#include "sdcard.h"
#include "TCA9554PWR.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#define SD_CLK  GPIO_NUM_2
#define SD_CMD  GPIO_NUM_1
#define SD_D0   GPIO_NUM_42

static bool mounted = false;

bool sdcardInit() {
    // Pull DAT3/CS high via IO expander (inactive state)
    Set_EXIO(EXIO_PIN4, High);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.clk = SD_CLK;
    slot_config.cmd = SD_CMD;
    slot_config.d0 = SD_D0;
    slot_config.width = 1;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sd", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        printf("SD card: not available (%s)\n", esp_err_to_name(ret));
        return false;
    }

    printf("SD card mounted:\n");
    sdmmc_card_print_info(stdout, card);
    mounted = true;
    return true;
}

bool sdcardMounted() {
    return mounted;
}
