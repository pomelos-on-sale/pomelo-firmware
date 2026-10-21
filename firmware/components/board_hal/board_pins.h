/**
 * @file board_pins.h
 * @brief Pomelo UI 全板物理引脚与总线映射表 (Board Pinmap)
 *
 * 本文件是硬件引脚定义的唯一真理之源 (Single Source of Truth)。
 * 适配不同硬件/开发板时，只需在此处修改对应外设的 GPIO 编号即可。
 */
#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/spi_master.h"

// =============================================================================
// 0. 硬件特性能力开关 (Hardware Capabilities & Feature Flags)
// 适配新开发板时：有对应硬件置 1，缺失对应硬件置 0 即可自动优雅降级！
// =============================================================================
#define BOARD_HAS_PMIC_AXP2101          1  // 1: 板载 AXP2101 PMIC; 0: 无 PMIC (直接 LDO/USB 供电)
#define BOARD_HAS_AUDIO_SPEAKER         1  // 1: 板载 I2S 硬件音频编解码器 (ES8311 + 功放); 0: 降级为 Null Audio 虚拟声卡
#define BOARD_HAS_RTC_PCF85063A         1  // 1: 板载外置硬件 RTC; 0: 降级为 ESP32-S3 内部晶振走时 + SNTP 校准
#define BOARD_HAS_SD_CARD               1  // 1: 板载 MicroSD 卡槽; 0: 禁用 SD 检测与驱动，释放 4KB 任务栈
#define BOARD_HAS_BUTTON_BOOT           1  // 1: 板载 BOOT 实体按键; 0: 忽略该按键
#define BOARD_HAS_BUTTON_EXT            1  // 1: 板载物理扩展/侧边按键; 0: 忽略该按键

// =============================================================================
// 1. 屏幕显示接口引脚 (Display / AMOLED CO5300 QSPI)
// =============================================================================
#define BOARD_PIN_LCD_SPI_HOST      SPI2_HOST
#define BOARD_PIN_LCD_CS            GPIO_NUM_12
#define BOARD_PIN_LCD_PCLK          GPIO_NUM_38
#define BOARD_PIN_LCD_DATA0         GPIO_NUM_4
#define BOARD_PIN_LCD_DATA1         GPIO_NUM_5
#define BOARD_PIN_LCD_DATA2         GPIO_NUM_6
#define BOARD_PIN_LCD_DATA3         GPIO_NUM_7
#define BOARD_PIN_LCD_RST           GPIO_NUM_39
#define BOARD_PIN_LCD_TE_ENABLED    0             // 1: 启用硬件 TE 引脚, 0: 禁用 (PCB TP301 未引出)
#define BOARD_PIN_LCD_TE            GPIO_NUM_NC   // 硬件 TE 测试点未引出至 GPIO
#define BOARD_PIN_LCD_BL            GPIO_NUM_NC   // AMOLED 自发光，无背光控制引脚
#define BOARD_DISPLAY_BITS_PER_PIXEL 16

// =============================================================================
// 2. 触控面板引脚 (Touch Panel / CST816 / CST9217)
// =============================================================================
#define BOARD_PIN_TOUCH_SCL         GPIO_NUM_14
#define BOARD_PIN_TOUCH_SDA         GPIO_NUM_15
#define BOARD_PIN_TOUCH_INT         GPIO_NUM_11
#define BOARD_PIN_TOUCH_RST         GPIO_NUM_40

// =============================================================================
// 3. 音频接口引脚 (Audio I2S & ES8311 Codec / NS4150 PA)
// =============================================================================
#define BOARD_PIN_I2S_NUM           I2S_NUM_1
#define BOARD_PIN_I2S_BCLK          GPIO_NUM_9
#define BOARD_PIN_I2S_WS            GPIO_NUM_45
#define BOARD_PIN_I2S_DOUT          GPIO_NUM_8
#define BOARD_PIN_I2S_DIN           GPIO_NUM_10
#define BOARD_PIN_I2S_MCLK          GPIO_NUM_42
#define BOARD_PIN_AUDIO_PA          GPIO_NUM_46
#define BOARD_PIN_AUDIO_ES8311_ADDR 0x18

// =============================================================================
// 4. 电源管理与 RTC 主 I2C 总线引脚 (AXP2101 PMIC & PCF85063A RTC)
// =============================================================================
#define BOARD_PIN_I2C_NUM           I2C_NUM_0
#define BOARD_PIN_I2C_SCL           GPIO_NUM_14
#define BOARD_PIN_I2C_SDA           GPIO_NUM_15

// =============================================================================
// 5. 实体按键引脚 (Buttons)
// =============================================================================
#define BOARD_PIN_BUTTON_BOOT       GPIO_NUM_0
#define BOARD_PIN_BUTTON_EXT        GPIO_NUM_18

// =============================================================================
// 6. MicroSD 存储卡引脚 (SD Card / SDMMC 1-line)
// =============================================================================
#define BOARD_PIN_SD_CLK            GPIO_NUM_2
#define BOARD_PIN_SD_CMD            GPIO_NUM_1
#define BOARD_PIN_SD_D0             GPIO_NUM_3
