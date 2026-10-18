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

#define BOARD_BUTTON_BOOT_GPIO   GPIO_NUM_0
#define BOARD_BUTTON_EXT_GPIO    GPIO_NUM_18
#define BOARD_TOUCH_INT_GPIO     GPIO_NUM_11

/**
 * @brief Initialize hardware GPIO button pins.
 */
esp_err_t board_button_init(void);

/**
 * @brief Resume touch task immediately after wake from light sleep.
 */
void hal_touch_resume(void);

/**
 * @brief Drain buffered touch queue and reset touched state.
 */
void hal_touch_drain(void);

#ifdef __cplusplus
}
#endif

#endif // BOARD_HAL_INTERNAL_H
