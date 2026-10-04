#include "board_hal_internal.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <stdbool.h>

#define BOARD_BUTTON_BOOT_GPIO   GPIO_NUM_0
#define BOARD_BUTTON_EXT_GPIO    GPIO_NUM_18
#define BUTTON_QUEUE_SIZE        16

static const char *TAG = "board_button";
static QueueHandle_t s_btn_queue = NULL;

static void IRAM_ATTR button_gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    hal_button_event_t ev = HAL_BUTTON_EVENT_NONE;

    if (gpio_num == BOARD_BUTTON_BOOT_GPIO) {
        ev = (gpio_get_level(BOARD_BUTTON_BOOT_GPIO) == 0)
                 ? HAL_BUTTON_EVENT_BOOT_PRESS
                 : HAL_BUTTON_EVENT_BOOT_RELEASE;
    } else if (gpio_num == BOARD_BUTTON_EXT_GPIO) {
        ev = (gpio_get_level(BOARD_BUTTON_EXT_GPIO) == 0)
                 ? HAL_BUTTON_EVENT_SIDE_PRESS
                 : HAL_BUTTON_EVENT_SIDE_RELEASE;
    }

    if (ev != HAL_BUTTON_EVENT_NONE && s_btn_queue) {
        BaseType_t high_task_wakeup = pdFALSE;
        xQueueSendFromISR(s_btn_queue, &ev, &high_task_wakeup);
        if (high_task_wakeup) {
            portYIELD_FROM_ISR();
        }
    }
}

esp_err_t board_button_init(void)
{
    if (!s_btn_queue) {
        s_btn_queue = xQueueCreate(BUTTON_QUEUE_SIZE, sizeof(hal_button_event_t));
        if (!s_btn_queue) {
            ESP_LOGE(TAG, "Failed to create button event queue");
            return ESP_ERR_NO_MEM;
        }
    }

    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON_BOOT_GPIO) | (1ULL << BOARD_BUTTON_EXT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t ret = gpio_config(&btn_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    // Register button ISR handlers (install service only if not already active)
    esp_err_t isr_add_ret = gpio_isr_handler_add(BOARD_BUTTON_BOOT_GPIO, button_gpio_isr_handler, (void *)BOARD_BUTTON_BOOT_GPIO);
    if (isr_add_ret == ESP_ERR_INVALID_STATE) {
        gpio_install_isr_service(0);
        gpio_isr_handler_add(BOARD_BUTTON_BOOT_GPIO, button_gpio_isr_handler, (void *)BOARD_BUTTON_BOOT_GPIO);
    }
    gpio_isr_handler_add(BOARD_BUTTON_EXT_GPIO, button_gpio_isr_handler, (void *)BOARD_BUTTON_EXT_GPIO);

    return ESP_OK;
}



bool hal_button_wait_event(hal_button_event_t *out_event, uint32_t timeout_ms)
{
    if (!s_btn_queue || !out_event) {
        return false;
    }
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_btn_queue, out_event, ticks) == pdTRUE;
}
