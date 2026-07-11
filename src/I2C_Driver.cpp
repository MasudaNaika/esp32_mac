#include "I2C_Driver.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

namespace {

constexpr int I2C_TIMEOUT_MS = 1000;

static const char *TAG = "i2c_driver";
static i2c_master_bus_handle_t s_i2c_bus = nullptr;

static i2c_master_dev_handle_t getDeviceHandle(uint8_t device_addr) {
    static uint8_t cached_addr = 0xff;
    static i2c_master_dev_handle_t cached_handle = nullptr;

    if (cached_handle != nullptr && cached_addr == device_addr) {
        return cached_handle;
    }

    if (cached_handle != nullptr) {
        i2c_master_bus_rm_device(cached_handle);
        cached_handle = nullptr;
        cached_addr = 0xff;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_addr,
        .scl_speed_hz = I2C_Frequency,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = 0,
        },
    };

    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &cached_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device 0x%02x: %s", device_addr, esp_err_to_name(err));
        return nullptr;
    }

    cached_addr = device_addr;
    return cached_handle;
}

}  // namespace

void I2C_Init(void) {
    if (s_i2c_bus != nullptr) {
        return;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = static_cast<gpio_num_t>(I2C_SDA_PIN),
        .scl_io_num = static_cast<gpio_num_t>(I2C_SCL_PIN),
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
            .allow_pd = 0,
        },
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2C bus: %s", esp_err_to_name(err));
    }
}

bool I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
    i2c_master_dev_handle_t device = getDeviceHandle(Driver_addr);
    if (device == nullptr) {
        return false;
    }

    esp_err_t err = i2c_master_transmit(device, &Reg_addr, 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C read address write failed addr=0x%02x reg=0x%02x: %s",
                 Driver_addr, Reg_addr, esp_err_to_name(err));
        return false;
    }

    err = i2c_master_receive(device, Reg_data, Length, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed addr=0x%02x reg=0x%02x: %s",
                 Driver_addr, Reg_addr, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
    i2c_master_dev_handle_t device = getDeviceHandle(Driver_addr);
    if (device == nullptr) {
        return false;
    }

    uint8_t buffer[17];
    if (Length > sizeof(buffer) - 1) {
        ESP_LOGE(TAG, "I2C write too large: %lu bytes", (unsigned long)Length);
        return false;
    }

    buffer[0] = Reg_addr;
    for (uint32_t i = 0; i < Length; ++i) {
        buffer[i + 1] = Reg_data[i];
    }

    esp_err_t err = i2c_master_transmit(device, buffer, Length + 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed addr=0x%02x reg=0x%02x: %s",
                 Driver_addr, Reg_addr, esp_err_to_name(err));
        return false;
    }
    return true;
}
