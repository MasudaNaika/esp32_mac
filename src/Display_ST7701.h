#pragma once
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
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
#define ESP_PANEL_LCD_RGB_TIMING_HBP              (50)
#define ESP_PANEL_LCD_RGB_TIMING_HFP              (50)
#define ESP_PANEL_LCD_RGB_TIMING_VPW              (10)
#define ESP_PANEL_LCD_RGB_TIMING_VBP              (30)
#define ESP_PANEL_LCD_RGB_TIMING_VFP              (30)
#define ESP_PANEL_LCD_RGB_PCLK_ACTIVE_NEG         (0)
#define ESP_PANEL_LCD_RGB_DATA_WIDTH              (16)
#define ESP_PANEL_LCD_RGB_PIXEL_BITS              (16)
#define ESP_PANEL_LCD_RGB_FRAME_BUF_NUM           (1)
#define ESP_PANEL_LCD_RGB_BOUNCE_BUF_SIZE         (8 * ESP_PANEL_LCD_WIDTH)

#define ESP_PANEL_LCD_PIN_NUM_RGB_HSYNC           GPIO_NUM_38
#define ESP_PANEL_LCD_PIN_NUM_RGB_VSYNC           GPIO_NUM_39
#define ESP_PANEL_LCD_PIN_NUM_RGB_DE              GPIO_NUM_40
#define ESP_PANEL_LCD_PIN_NUM_RGB_PCLK            GPIO_NUM_41
#define ESP_PANEL_LCD_PIN_NUM_RGB_DISP            GPIO_NUM_NC
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA0           GPIO_NUM_5
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA1           GPIO_NUM_45
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA2           GPIO_NUM_48
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA3           GPIO_NUM_47
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA4           GPIO_NUM_21
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA5           GPIO_NUM_14
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA6           GPIO_NUM_13
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA7           GPIO_NUM_12
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA8           GPIO_NUM_11
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA9           GPIO_NUM_10
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA10          GPIO_NUM_9
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA11          GPIO_NUM_46
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA12          GPIO_NUM_3
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA13          GPIO_NUM_8
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA14          GPIO_NUM_18
#define ESP_PANEL_LCD_PIN_NUM_RGB_DATA15          GPIO_NUM_17

extern esp_lcd_panel_handle_t panel_handle;
extern uint8_t LCD_Backlight;

void ST7701_Reset();
void ST7701_Init();
void LCD_Init();
void LCD_SetMacFrameBuffer(const uint8_t *framebuffer, bool rotate180);
void Backlight_Init();
void Set_Backlight(uint8_t Light);
void LCD_FreeSPI();
