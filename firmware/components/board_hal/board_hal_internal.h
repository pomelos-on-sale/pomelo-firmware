#ifndef BOARD_HAL_INTERNAL_H
#define BOARD_HAL_INTERNAL_H

#include "board_hal.h"
#include "board_pins.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get or initialize shared hardware I2C master bus handle.
 */
i2c_master_bus_handle_t hal_i2c_get_master_bus(void);

/**
 * @brief Initialize AMOLED display and Ping-Pong DMA buffers.
 */
esp_err_t board_display_init(void);

/**
 * @brief Initialize CST9217 touch controller.
 */
esp_err_t board_touch_init(void);

/**
 * @brief Initialize hardware GPIO button pins.
 */
esp_err_t board_button_init(void);

#ifdef __cplusplus
}
#endif

#endif // BOARD_HAL_INTERNAL_H
