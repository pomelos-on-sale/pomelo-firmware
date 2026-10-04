#include "board_hal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "board_event";

#define HAL_EVENT_QUEUE_SIZE 32

static QueueHandle_t s_event_queue = NULL;

esp_err_t hal_event_init(void)
{
    if (s_event_queue != NULL) {
        return ESP_OK;
    }
    s_event_queue = xQueueCreate(HAL_EVENT_QUEUE_SIZE, sizeof(hal_event_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Failed to create unified hardware event queue");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Unified hardware event queue initialized (size=%d)", HAL_EVENT_QUEUE_SIZE);
    return ESP_OK;
}

bool hal_event_send(const hal_event_t *ev)
{
    if (!s_event_queue && hal_event_init() != ESP_OK) {
        return false;
    }
    if (!ev) {
        return false;
    }
    return xQueueSend(s_event_queue, ev, 0) == pdTRUE;
}

bool hal_event_send_from_isr(const hal_event_t *ev)
{
    if (!s_event_queue || !ev) {
        return false;
    }
    BaseType_t high_task_wakeup = pdFALSE;
    BaseType_t res = xQueueSendFromISR(s_event_queue, ev, &high_task_wakeup);
    if (high_task_wakeup) {
        portYIELD_FROM_ISR();
    }
    return res == pdTRUE;
}

bool hal_event_wait(hal_event_t *out_event, uint32_t timeout_ms)
{
    if (!s_event_queue && hal_event_init() != ESP_OK) {
        return false;
    }
    if (!out_event) {
        return false;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_event_queue, out_event, ticks) == pdTRUE;
}
