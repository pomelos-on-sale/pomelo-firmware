#include "board_hal_internal.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdbool.h>

static void IRAM_ATTR button_gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    hal_button_event_t ev = HAL_BUTTON_EVENT_NONE;

    if (gpio_num == BOARD_PIN_BUTTON_BOOT) {
        ev = (gpio_get_level(BOARD_PIN_BUTTON_BOOT) == 0)
                 ? HAL_BUTTON_EVENT_BOOT_PRESS
                 : HAL_BUTTON_EVENT_BOOT_RELEASE;
    } else if (gpio_num == BOARD_PIN_BUTTON_EXT) {
        ev = (gpio_get_level(BOARD_PIN_BUTTON_EXT) == 0)
                 ? HAL_BUTTON_EVENT_SIDE_PRESS
                 : HAL_BUTTON_EVENT_SIDE_RELEASE;
    }

    if (ev != HAL_BUTTON_EVENT_NONE) {
        hal_event_t sys_ev = {
            .type = HAL_EVENT_BUTTON,
            .data.button = ev,
        };
        hal_event_send_from_isr(&sys_ev);
    }
}

esp_err_t board_button_init(void)
{
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BUTTON_BOOT) | (1ULL << BOARD_PIN_BUTTON_EXT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t ret = gpio_config(&btn_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    // Register button ISR handlers (service initialized globally in board_hal_init)
    gpio_isr_handler_add(BOARD_PIN_BUTTON_BOOT, button_gpio_isr_handler, (void *)BOARD_PIN_BUTTON_BOOT);
    gpio_isr_handler_add(BOARD_PIN_BUTTON_EXT, button_gpio_isr_handler, (void *)BOARD_PIN_BUTTON_EXT);

    return ESP_OK;
}

