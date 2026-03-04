#pragma once
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"

#include "TCA9554PWR.h"

#define LCD_CLK_PIN   2
#define LCD_MOSI_PIN  1
#define LCD_Backlight_PIN   6
#define PWM_Channel     1
#define Frequency       20000
#define Resolution      10
#define Dutyfactor      500
#define Backlight_MAX   100

#define ESP_PANEL_LCD_WIDTH                       (480)
#define ESP_PANEL_LCD_HEIGHT                      (640)
#define ESP_PANEL_LCD_RGB_TIMING_FREQ_HZ          (12 * 1000 * 1000)
#define ESP_PANEL_LCD_RGB_TIMING_HPW              (10)
#define ESP_PANEL_LCD_RGB_TIMING_HBP              (70)
#define ESP_PANEL_LCD_RGB_TIMING_HFP              (60)
#define ESP_PANEL_LCD_RGB_TIMING_VPW              (10)
#define ESP_PANEL_LCD_RGB_TIMING_VBP              (40)
#define ESP_PANEL_LCD_RGB_TIMING_VFP              (40)
#define ESP_PANEL_LCD_RGB_PCLK_ACTIVE_NEG         (0)
#define ESP_PANEL_LCD_RGB_DATA_WIDTH              (16)
#define ESP_PANEL_LCD_RGB_PIXEL_BITS              (16)
#define ESP_PANEL_LCD_RGB_FRAME_BUF_NUM           (1)
#define ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE         (10 * ESP_PANEL_LCD_HEIGHT)

#define ESP_PANEL_LCD_PIN_NUM_RGB_HSYNC           (38)
#define ESP_PANEL_LCD_PIN_NUM_RGB_VSYNC           (39)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DE              (40)
#define ESP_PANEL_LCD_PIN_NUM_RGB_PCLK            (41)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DISP            (-1)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA0           (5)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA1           (45)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA2           (48)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA3           (47)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA4           (21)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA5           (14)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA6           (13)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA7           (12)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA8           (11)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA9           (10)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA10          (9)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA11          (46)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA12          (3)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA13          (8)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA14          (18)
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA15          (17)

extern esp_lcd_panel_handle_t panel_handle;
extern uint8_t LCD_Backlight;

void ST7701_Reset();
void ST7701_Init();
void LCD_Init();
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint8_t* color);
void Backlight_Init();
void Set_Backlight(uint8_t Light);
