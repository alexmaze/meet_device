#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_REFERENCE       true

// Native 16 kHz for AFE / Meet uplink. If ES8388 fails on hardware, revert both to 24000
// and move resampler to the uplink path (see ADR / plan M2.1).
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_38
#define AUDIO_I2S_GPIO_WS GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_14
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_12
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_45

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_NC
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_2
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_1
#define AUDIO_CODEC_ES8388_ADDR  0x20

#define STATE_OUTPUT_GPIO       GPIO_NUM_47
#define BOOT_BUTTON_GPIO        GPIO_NUM_11

#define BATTERY_ADC_CHANNEL      ADC_CHANNEL_2  // GPIO3
#define BATTERY_REF_ADC_CHANNEL  ADC_CHANNEL_3  // GPIO4
#define VOLUME_KEY_ADC_CHANNEL   ADC_CHANNEL_8  // GPIO9
#define VOLUME_DOWN_KEY_MIN_MV   0
#define VOLUME_DOWN_KEY_MAX_MV   700
#define VOLUME_UP_KEY_MIN_MV     900
#define VOLUME_UP_KEY_MAX_MV     3000
#define BATTERY_REF_VOLTAGE_MV   1270

#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_42
#define DISPLAY_MOSI_PIN      GPIO_NUM_41
#define DISPLAY_CLK_PIN       GPIO_NUM_46
#define DISPLAY_DC_PIN        GPIO_NUM_39
#define DISPLAY_RST_PIN       GPIO_NUM_NC
#define DISPLAY_CS_PIN        GPIO_NUM_40

#define LCD_TYPE_ST7789_SERIAL
#define DISPLAY_WIDTH   240
#define DISPLAY_HEIGHT  320
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false

#define DISPLAY_WIDTH_1  320
#define DISPLAY_HEIGHT_1 240
// Xiaozhi _1 is MX=true,MY=false. Meet uses hardware landscape without LVGL
// software rotation, so that combination appears 180° inverted on 太空舱.
#define DISPLAY_MIRROR_X_1 false
#define DISPLAY_MIRROR_Y_1 true
#define DISPLAY_SWAP_XY_1 true

#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true
#define DISPLAY_SPI_MODE 0

#endif // _BOARD_CONFIG_H_
