#include "Display_ST7701.h"

spi_device_handle_t SPI_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;
uint8_t LCD_Backlight = 100;

namespace {

constexpr ledc_mode_t kBacklightMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kBacklightTimer = LEDC_TIMER_1;
constexpr ledc_channel_t kBacklightChannel = LEDC_CHANNEL_1;

static void delayMs(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}

constexpr int kMacFramebufferStride = 640 / 8;
static_assert(ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE == 8 * ESP_PANEL_LCD_WIDTH,
              "direct Mac VRAM fill requires an eight-line bounce buffer");
static_assert((ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE % 8) == 0,
              "bounce buffer must contain a whole number of packed bytes");
static_assert(((ESP_PANEL_LCD_WIDTH * ESP_PANEL_LCD_HEIGHT) %
               ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE) == 0,
              "bounce buffer must divide the frame exactly");

static DRAM_ATTR uint32_t pixel_pair_first_lut[256][8];
static DRAM_ATTR uint32_t pixel_pair_second_lut[256][8];
static DRAM_ATTR uint8_t mac_x_byte_lut[ESP_PANEL_LCD_HEIGHT / 8];
static DRAM_ATTR const uint8_t *volatile mac_framebuffer = NULL;
static DRAM_ATTR int output_pair_start = 0;
static DRAM_ATTR int output_pair_step = 1;
static bool bounce_lut_initialized = false;
static bool bounce_lut_rotate_180 = false;

static IRAM_ATTR bool fillBounceBuffer(esp_lcd_panel_handle_t panel, void *bounce_buf,
                                       int pos_px, int len_bytes, void *user_ctx) {
  (void)panel;
  (void)len_bytes;
  (void)user_ctx;
  const uint8_t *mac_fb = mac_framebuffer;
  if (!mac_fb) {
    return false;
  }

  uint32_t *pixel_pairs = static_cast<uint32_t *>(bounce_buf);
  const int chunk = (pos_px / ESP_PANEL_LCD_WIDTH) >> 3;
  const int byte_index = mac_x_byte_lut[chunk];
  uint32_t *out = pixel_pairs + output_pair_start;
  const int out_step = output_pair_step;

  // One packed Mac byte contains the pixels for all eight LCD rows in this
  // bounce buffer. Two adjacent pixels are emitted with each 32-bit store.
  for (int mac_y = 0; mac_y < ESP_PANEL_LCD_WIDTH; mac_y += 2) {
    const uint32_t *colors0 = pixel_pair_first_lut[
        mac_fb[mac_y * kMacFramebufferStride + byte_index]];
    const uint32_t *colors1 = pixel_pair_second_lut[
        mac_fb[(mac_y + 1) * kMacFramebufferStride + byte_index]];

    out[0 * (ESP_PANEL_LCD_WIDTH / 2)] = colors0[0] | colors1[0];
    out[1 * (ESP_PANEL_LCD_WIDTH / 2)] = colors0[1] | colors1[1];
    out[2 * (ESP_PANEL_LCD_WIDTH / 2)] = colors0[2] | colors1[2];
    out[3 * (ESP_PANEL_LCD_WIDTH / 2)] = colors0[3] | colors1[3];
    out[4 * (ESP_PANEL_LCD_WIDTH / 2)] = colors0[4] | colors1[4];
    out[5 * (ESP_PANEL_LCD_WIDTH / 2)] = colors0[5] | colors1[5];
    out[6 * (ESP_PANEL_LCD_WIDTH / 2)] = colors0[6] | colors1[6];
    out[7 * (ESP_PANEL_LCD_WIDTH / 2)] = colors0[7] | colors1[7];
    out += out_step;
  }
  return false;
}

static void buildBounceLut(bool rotate180) {
  output_pair_start = rotate180 ? 0 : (ESP_PANEL_LCD_WIDTH / 2) - 1;
  output_pair_step = rotate180 ? 1 : -1;

  for (int chunk = 0; chunk < ESP_PANEL_LCD_HEIGHT / 8; ++chunk) {
    const int first_panel_y = chunk * 8;
    const int first_mac_x = rotate180
        ? (ESP_PANEL_LCD_HEIGHT - 1) - first_panel_y
        : first_panel_y;
    mac_x_byte_lut[chunk] = first_mac_x >> 3;
  }

  for (int packed = 0; packed < 256; ++packed) {
    for (int row = 0; row < 8; ++row) {
      const int bit = rotate180 ? 7 - row : row;
      const uint32_t pixel = (packed & (0x80u >> bit)) ? 0x0000u : 0xFFFFu;
      pixel_pair_first_lut[packed][row] = rotate180 ? pixel : (pixel << 16);
      pixel_pair_second_lut[packed][row] = rotate180 ? (pixel << 16) : pixel;
    }
  }
  bounce_lut_rotate_180 = rotate180;
  bounce_lut_initialized = true;
}

}

void ST7701_WriteCommand(uint8_t cmd)
{
  spi_transaction_t spi_tran = {};
  spi_tran.cmd = 0;
  spi_tran.addr = cmd;
  spi_device_transmit(SPI_handle, &spi_tran);
}

void ST7701_WriteData(uint8_t data)
{
  spi_transaction_t spi_tran = {};
  spi_tran.cmd = 1;
  spi_tran.addr = data;
  spi_device_transmit(SPI_handle, &spi_tran);
}

void ST7701_CS_EN() {
  Set_EXIO(EXIO_PIN3, Low);
  vTaskDelay(pdMS_TO_TICKS(10));
}

void ST7701_CS_Dis() {
  Set_EXIO(EXIO_PIN3, High);
  vTaskDelay(pdMS_TO_TICKS(10));
}

void ST7701_Reset() {
  Set_EXIO(EXIO_PIN1, Low);
  vTaskDelay(pdMS_TO_TICKS(10));
  Set_EXIO(EXIO_PIN1, High);
  vTaskDelay(pdMS_TO_TICKS(50));
}

void ST7701_Init()
{
  spi_bus_config_t buscfg = {};
  buscfg.mosi_io_num = LCD_MOSI_PIN;
  buscfg.miso_io_num = -1;
  buscfg.sclk_io_num = LCD_CLK_PIN;
  buscfg.quadwp_io_num = -1;
  buscfg.quadhd_io_num = -1;
  buscfg.max_transfer_sz = 64;
  spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
  spi_device_interface_config_t devcfg = {};
  devcfg.command_bits = 1;
  devcfg.address_bits = 8;
  devcfg.mode = 0;
  devcfg.clock_speed_hz = 80000000;
  devcfg.spics_io_num = -1;
  devcfg.queue_size = 1;
  spi_bus_add_device(SPI2_HOST, &devcfg, &SPI_handle);

  ST7701_CS_EN();

  // ST7701 2.8inch init sequence
  ST7701_WriteCommand(0xFF);ST7701_WriteData(0x77);ST7701_WriteData(0x01);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x13);
  ST7701_WriteCommand(0xEF);ST7701_WriteData(0x08);
  ST7701_WriteCommand(0xFF);ST7701_WriteData(0x77);ST7701_WriteData(0x01);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x10);
  ST7701_WriteCommand(0xC0);ST7701_WriteData(0x4F);ST7701_WriteData(0x00);
  ST7701_WriteCommand(0xC1);ST7701_WriteData(0x10);ST7701_WriteData(0x02);
  ST7701_WriteCommand(0xC2);ST7701_WriteData(0x07);ST7701_WriteData(0x02);
  ST7701_WriteCommand(0xCC);ST7701_WriteData(0x10);
  ST7701_WriteCommand(0xB0);ST7701_WriteData(0x00);ST7701_WriteData(0x10);ST7701_WriteData(0x17);ST7701_WriteData(0x0D);ST7701_WriteData(0x11);ST7701_WriteData(0x06);ST7701_WriteData(0x05);ST7701_WriteData(0x08);ST7701_WriteData(0x07);ST7701_WriteData(0x1F);ST7701_WriteData(0x04);ST7701_WriteData(0x11);ST7701_WriteData(0x0E);ST7701_WriteData(0x29);ST7701_WriteData(0x30);ST7701_WriteData(0x1F);
  ST7701_WriteCommand(0xB1);ST7701_WriteData(0x00);ST7701_WriteData(0x0D);ST7701_WriteData(0x14);ST7701_WriteData(0x0E);ST7701_WriteData(0x11);ST7701_WriteData(0x06);ST7701_WriteData(0x04);ST7701_WriteData(0x08);ST7701_WriteData(0x08);ST7701_WriteData(0x20);ST7701_WriteData(0x05);ST7701_WriteData(0x13);ST7701_WriteData(0x13);ST7701_WriteData(0x26);ST7701_WriteData(0x30);ST7701_WriteData(0x1F);
  ST7701_WriteCommand(0xFF);ST7701_WriteData(0x77);ST7701_WriteData(0x01);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x11);
  ST7701_WriteCommand(0xB0);ST7701_WriteData(0x65);
  ST7701_WriteCommand(0xB1);ST7701_WriteData(0x71);
  ST7701_WriteCommand(0xB2);ST7701_WriteData(0x82);
  ST7701_WriteCommand(0xB3);ST7701_WriteData(0x80);
  ST7701_WriteCommand(0xB5);ST7701_WriteData(0x42);
  ST7701_WriteCommand(0xB7);ST7701_WriteData(0x85);
  ST7701_WriteCommand(0xB8);ST7701_WriteData(0x20);
  ST7701_WriteCommand(0xC0);ST7701_WriteData(0x09);
  ST7701_WriteCommand(0xC1);ST7701_WriteData(0x78);
  ST7701_WriteCommand(0xC2);ST7701_WriteData(0x78);
  ST7701_WriteCommand(0xD0);ST7701_WriteData(0x88);
  ST7701_WriteCommand(0xEE);ST7701_WriteData(0x42);

  ST7701_WriteCommand(0xE0);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x02);
  ST7701_WriteCommand(0xE1);ST7701_WriteData(0x04);ST7701_WriteData(0xA0);ST7701_WriteData(0x06);ST7701_WriteData(0xA0);ST7701_WriteData(0x05);ST7701_WriteData(0xA0);ST7701_WriteData(0x07);ST7701_WriteData(0xA0);ST7701_WriteData(0x00);ST7701_WriteData(0x44);ST7701_WriteData(0x44);
  ST7701_WriteCommand(0xE2);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);
  ST7701_WriteCommand(0xE3);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x22);ST7701_WriteData(0x22);
  ST7701_WriteCommand(0xE4);ST7701_WriteData(0x44);ST7701_WriteData(0x44);
  ST7701_WriteCommand(0xE5);ST7701_WriteData(0x0c);ST7701_WriteData(0x90);ST7701_WriteData(0xA0);ST7701_WriteData(0xA0);ST7701_WriteData(0x0E);ST7701_WriteData(0x92);ST7701_WriteData(0xA0);ST7701_WriteData(0xA0);ST7701_WriteData(0x08);ST7701_WriteData(0x8C);ST7701_WriteData(0xA0);ST7701_WriteData(0xA0);ST7701_WriteData(0x0A);ST7701_WriteData(0x8E);ST7701_WriteData(0xA0);ST7701_WriteData(0xA0);
  ST7701_WriteCommand(0xE6);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x22);ST7701_WriteData(0x22);
  ST7701_WriteCommand(0xE7);ST7701_WriteData(0x44);ST7701_WriteData(0x44);
  ST7701_WriteCommand(0xE8);ST7701_WriteData(0x0D);ST7701_WriteData(0x91);ST7701_WriteData(0xA0);ST7701_WriteData(0xA0);ST7701_WriteData(0x0F);ST7701_WriteData(0x93);ST7701_WriteData(0xA0);ST7701_WriteData(0xA0);ST7701_WriteData(0x09);ST7701_WriteData(0x8D);ST7701_WriteData(0xA0);ST7701_WriteData(0xA0);ST7701_WriteData(0x0B);ST7701_WriteData(0x8F);ST7701_WriteData(0xA0);ST7701_WriteData(0xA0);
  ST7701_WriteCommand(0xEB);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0xE4);ST7701_WriteData(0xE4);ST7701_WriteData(0x44);ST7701_WriteData(0x00);ST7701_WriteData(0x40);
  ST7701_WriteCommand(0xED);ST7701_WriteData(0xFF);ST7701_WriteData(0xF5);ST7701_WriteData(0x47);ST7701_WriteData(0x6F);ST7701_WriteData(0x0B);ST7701_WriteData(0xA1);ST7701_WriteData(0xAB);ST7701_WriteData(0xFF);ST7701_WriteData(0xFF);ST7701_WriteData(0xBA);ST7701_WriteData(0x1A);ST7701_WriteData(0xB0);ST7701_WriteData(0xF6);ST7701_WriteData(0x74);ST7701_WriteData(0x5F);ST7701_WriteData(0xFF);
  ST7701_WriteCommand(0xEF);ST7701_WriteData(0x08);ST7701_WriteData(0x08);ST7701_WriteData(0x08);ST7701_WriteData(0x40);ST7701_WriteData(0x3F);ST7701_WriteData(0x64);
  ST7701_WriteCommand(0xFF);ST7701_WriteData(0x77);ST7701_WriteData(0x01);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);
  ST7701_WriteCommand(0xFF);ST7701_WriteData(0x77);ST7701_WriteData(0x01);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x13);
  ST7701_WriteCommand(0xE6);ST7701_WriteData(0x16);ST7701_WriteData(0x7C);
  ST7701_WriteCommand(0xE8);ST7701_WriteData(0x00);ST7701_WriteData(0x0E);
  ST7701_WriteCommand(0xFF);ST7701_WriteData(0x77);ST7701_WriteData(0x01);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);
  ST7701_WriteCommand(0x11);ST7701_WriteData(0x00);
  delayMs(200);
  ST7701_WriteCommand(0xFF);ST7701_WriteData(0x77);ST7701_WriteData(0x01);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x13);
  ST7701_WriteCommand(0xE8);ST7701_WriteData(0x00);ST7701_WriteData(0x0C);
  delayMs(150);
  ST7701_WriteCommand(0xE8);ST7701_WriteData(0x00);ST7701_WriteData(0x00);
  ST7701_WriteCommand(0xFF);ST7701_WriteData(0x77);ST7701_WriteData(0x01);ST7701_WriteData(0x00);ST7701_WriteData(0x00);ST7701_WriteData(0x00);
  ST7701_WriteCommand(0x29);ST7701_WriteData(0x00);
  ST7701_WriteCommand(0x35);ST7701_WriteData(0x00);

  ST7701_WriteCommand(0x11);
  ST7701_WriteData(0x00);  // sleep out
  delayMs(200);

  ST7701_WriteCommand(0x29);
  ST7701_WriteData(0x00);  // display on
  ST7701_WriteCommand(0x29);
  ST7701_WriteData(0x00);  // display on
  delayMs(100);

  ST7701_CS_Dis();

  // RGB panel config - field order must match esp_lcd_rgb_panel_config_t
  esp_lcd_rgb_panel_config_t rgb_config;
  memset(&rgb_config, 0, sizeof(rgb_config));
  rgb_config.clk_src = LCD_CLK_SRC_PLL240M;
  rgb_config.timings.pclk_hz = ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ;
  rgb_config.timings.h_res = ESP_PANEL_LCD_WIDTH;
  rgb_config.timings.v_res = ESP_PANEL_LCD_HEIGHT;
  rgb_config.timings.hsync_pulse_width = ESP_PANEL_LCD_RGB_TIMING_HPW;
  rgb_config.timings.hsync_back_porch = ESP_PANEL_LCD_RGB_TIMING_HBP;
  rgb_config.timings.hsync_front_porch = ESP_PANEL_LCD_RGB_TIMING_HFP;
  rgb_config.timings.vsync_pulse_width = ESP_PANEL_LCD_RGB_TIMING_VPW;
  rgb_config.timings.vsync_back_porch = ESP_PANEL_LCD_RGB_TIMING_VBP;
  rgb_config.timings.vsync_front_porch = ESP_PANEL_LCD_RGB_TIMING_VFP;
  rgb_config.timings.flags.pclk_active_neg = ESP_PANEL_LCD_RGB_PCLK_ACTIVE_NEG;
  rgb_config.data_width = ESP_PANEL_LCD_RGB_DATA_WIDTH;
  rgb_config.in_color_format = static_cast<lcd_color_format_t>(ESP_COLOR_FOURCC_RGB16);
  rgb_config.out_color_format = static_cast<lcd_color_format_t>(ESP_COLOR_FOURCC_RGB16);
  rgb_config.num_fbs = 0;
  rgb_config.bounce_buffer_size_px = ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE;
  rgb_config.dma_burst_size = 64;
  rgb_config.hsync_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_HSYNC;
  rgb_config.vsync_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_VSYNC;
  rgb_config.de_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_DE;
  rgb_config.pclk_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_PCLK;
  rgb_config.data_gpio_nums[0] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA0;
  rgb_config.data_gpio_nums[1] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA1;
  rgb_config.data_gpio_nums[2] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA2;
  rgb_config.data_gpio_nums[3] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA3;
  rgb_config.data_gpio_nums[4] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA4;
  rgb_config.data_gpio_nums[5] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA5;
  rgb_config.data_gpio_nums[6] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA6;
  rgb_config.data_gpio_nums[7] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA7;
  rgb_config.data_gpio_nums[8] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA8;
  rgb_config.data_gpio_nums[9] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA9;
  rgb_config.data_gpio_nums[10] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA10;
  rgb_config.data_gpio_nums[11] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA11;
  rgb_config.data_gpio_nums[12] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA12;
  rgb_config.data_gpio_nums[13] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA13;
  rgb_config.data_gpio_nums[14] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA14;
  rgb_config.data_gpio_nums[15] = ESP_PANEL_LCD_PIN_NUM_RGB_DATA15;
  rgb_config.disp_gpio_num = ESP_PANEL_LCD_PIN_NUM_RGB_DISP;
  printf("Internal DMA heap before RGB panel: free=%d largest=%d bounce=%d\n",
         (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
         (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
         (int)(ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE * sizeof(uint16_t) * 2));
  if (!bounce_lut_initialized) {
    buildBounceLut(false);
  }
  rgb_config.flags.no_fb = true;
  esp_err_t err = esp_lcd_new_rgb_panel(&rgb_config, &panel_handle);
  printf("esp_lcd_new_rgb_panel: %s (0x%x) panel=%p\n", esp_err_to_name(err), err, panel_handle);
  printf("Internal DMA heap after RGB panel: free=%d largest=%d\n",
         (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
         (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  if (err == ESP_OK && panel_handle) {
    esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
    callbacks.on_bounce_empty = fillBounceBuffer;
    err = esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &callbacks, NULL);
    printf("esp_lcd_rgb_panel_register_event_callbacks: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) {
      return;
    }
    err = esp_lcd_panel_reset(panel_handle);
    printf("esp_lcd_panel_reset: %s\n", esp_err_to_name(err));
    err = esp_lcd_panel_init(panel_handle);
    printf("esp_lcd_panel_init: %s\n", esp_err_to_name(err));

    printf("RGB bounce-only front buffer: %p\n", mac_framebuffer);
  }
}

void LCD_SetMacFrameBuffer(const uint8_t *framebuffer, bool rotate180) {
  if (!bounce_lut_initialized || bounce_lut_rotate_180 != rotate180) {
    buildBounceLut(rotate180);
  }
  mac_framebuffer = framebuffer;
}

void LCD_Init() {
  ST7701_Reset();
  ST7701_Init();
}

void LCD_FreeSPI() {
  if (SPI_handle) {
    spi_bus_remove_device(SPI_handle);
    SPI_handle = NULL;
  }
  spi_bus_free(SPI2_HOST);
  printf("LCD SPI bus freed (GPIO1/GPIO2 available for SD card)\n");
}

void Backlight_Init()
{
  ledc_timer_config_t timer_cfg = {};
  timer_cfg.speed_mode = kBacklightMode;
  timer_cfg.duty_resolution = static_cast<ledc_timer_bit_t>(Resolution);
  timer_cfg.timer_num = kBacklightTimer;
  timer_cfg.freq_hz = Frequency;
  timer_cfg.clk_cfg = LEDC_AUTO_CLK;

  ledc_channel_config_t channel_cfg = {};
  channel_cfg.gpio_num = LCD_Backlight_PIN;
  channel_cfg.speed_mode = kBacklightMode;
  channel_cfg.channel = kBacklightChannel;
  channel_cfg.timer_sel = kBacklightTimer;
  channel_cfg.duty = Dutyfactor;
  channel_cfg.hpoint = 0;
  channel_cfg.sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD;
  channel_cfg.flags.output_invert = 0;

  ledc_timer_config(&timer_cfg);
  ledc_channel_config(&channel_cfg);
  Set_Backlight(LCD_Backlight);
}

void Set_Backlight(uint8_t Light)
{
  if (Light > Backlight_MAX)
    printf("Set Backlight parameters in the range of 0 to 100\r\n");
  else {
    uint32_t bl = Light * 10;
    if (bl == 1000)
      bl = 1024;
    ledc_set_duty(kBacklightMode, kBacklightChannel, bl);
    ledc_update_duty(kBacklightMode, kBacklightChannel);
    LCD_Backlight = Light;
  }
}
