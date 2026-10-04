#ifndef BOARD_HAL_INTERNAL_H
#define BOARD_HAL_INTERNAL_H

#include "board_hal.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize AMOLED display and Ping-Pong DMA buffers.
 */
esp_err_t board_display_init(void);

/**
 * @brief Initialize CST816 touch controller.
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
