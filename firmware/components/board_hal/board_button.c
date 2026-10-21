#include "board_hal_internal.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdbool.h>

#if BOARD_HAS_BUTTON_BOOT || BOARD_HAS_BUTTON_EXT

static void IRAM_ATTR button_gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    hal_button_event_t ev = HAL_BUTTON_EVENT_NONE;

#if BOARD_HAS_BUTTON_BOOT
    if (BOARD_PIN_BUTTON_BOOT != GPIO_NUM_NC && gpio_num == (uint32_t)BOARD_PIN_BUTTON_BOOT) {
        ev = (gpio_get_level(BOARD_PIN_BUTTON_BOOT) == 0)
                 ? HAL_BUTTON_EVENT_BOOT_PRESS
                 : HAL_BUTTON_EVENT_BOOT_RELEASE;
    }
#endif

#if BOARD_HAS_BUTTON_EXT
    if (BOARD_PIN_BUTTON_EXT != GPIO_NUM_NC && gpio_num == (uint32_t)BOARD_PIN_BUTTON_EXT) {
        ev = (gpio_get_level(BOARD_PIN_BUTTON_EXT) == 0)
                 ? HAL_BUTTON_EVENT_SIDE_PRESS
                 : HAL_BUTTON_EVENT_SIDE_RELEASE;
    }
#endif

    if (ev != HAL_BUTTON_EVENT_NONE) {
        hal_event_t sys_ev = {
            .type = HAL_EVENT_BUTTON,
            .data.button = ev,
        };
        hal_event_send_from_isr(&sys_ev);
    }
}

#endif // BOARD_HAS_BUTTON_BOOT || BOARD_HAS_BUTTON_EXT

esp_err_t board_button_init(void)
{
#if BOARD_HAS_BUTTON_BOOT || BOARD_HAS_BUTTON_EXT
    uint64_t pin_mask = 0;

#if BOARD_HAS_BUTTON_BOOT
    if (BOARD_PIN_BUTTON_BOOT != GPIO_NUM_NC) {
        pin_mask |= (1ULL << BOARD_PIN_BUTTON_BOOT);
    }
#endif

#if BOARD_HAS_BUTTON_EXT
    if (BOARD_PIN_BUTTON_EXT != GPIO_NUM_NC) {
        pin_mask |= (1ULL << BOARD_PIN_BUTTON_EXT);
    }
#endif

    if (pin_mask == 0) {
        return ESP_OK;
    }

    gpio_config_t btn_cfg = {
        .pin_bit_mask = pin_mask,
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
#if BOARD_HAS_BUTTON_BOOT
    if (BOARD_PIN_BUTTON_BOOT != GPIO_NUM_NC) {
        gpio_isr_handler_add(BOARD_PIN_BUTTON_BOOT, button_gpio_isr_handler, (void *)(uint32_t)BOARD_PIN_BUTTON_BOOT);
    }
#endif

#if BOARD_HAS_BUTTON_EXT
    if (BOARD_PIN_BUTTON_EXT != GPIO_NUM_NC) {
        gpio_isr_handler_add(BOARD_PIN_BUTTON_EXT, button_gpio_isr_handler, (void *)(uint32_t)BOARD_PIN_BUTTON_EXT);
    }
#endif

    return ESP_OK;
#else
    ESP_LOGI("board_button", "Physical buttons bypassed: no hardware buttons configured");
    return ESP_OK;
#endif
}
