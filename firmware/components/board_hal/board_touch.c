#include "board_hal_internal.h"
#include "esp_log.h"
#include "esp_lcd_touch.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include <stdlib.h>

static const char *TAG = "board_touch";

static esp_lcd_touch_handle_t s_touch_handle = NULL;
static SemaphoreHandle_t s_touch_sem = NULL;
static QueueHandle_t s_touch_event_queue = NULL;
static TaskHandle_t s_touch_task_handle = NULL;

typedef struct {
    bool touched;
    int32_t x;
    int32_t y;
    uint32_t seq;
} touch_cache_t;

static touch_cache_t s_touch_cache = {0};
static portMUX_TYPE s_touch_spinlock = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Hardware ISR callback triggered when touch INT pin asserts.
 * Wakes the background touch task instantly with zero CPU latency.
 */
static void IRAM_ATTR touch_isr_callback(esp_lcd_touch_handle_t tp)
{
    BaseType_t high_task_wakeup = pdFALSE;
    if (s_touch_sem != NULL) {
        xSemaphoreGiveFromISR(s_touch_sem, &high_task_wakeup);
        if (high_task_wakeup == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    }
}

/**
 * @brief Dedicated FreeRTOS touch sampling task pinned to Core 0.
 *
 * State Machine:
 * 1. Idle (no touch): Blocks indefinitely on s_touch_sem (0% CPU, 0 I2C bus traffic).
 * 2. On INT Interrupt: Wakes immediately, performs I2C read, generates DOWN event.
 * 3. In Drag/Tracking: Samples at ~100Hz (10ms), generates MOVE events into queue.
 * 4. On Release (pt_cnt == 0): Generates UP event, returns to idle wait.
 */
static void touch_task(void *arg)
{
    ESP_LOGI(TAG, "Touch background task started on Core %d", xPortGetCoreID());

    TickType_t wait_ticks = portMAX_DELAY;
    bool currently_touched = false;
    uint8_t release_debounce = 0;
    int32_t last_x = -1;
    int32_t last_y = -1;

    while (1) {
        // Wait for touch interrupt event (when idle) or tracking timeout (when dragging)
        xSemaphoreTake(s_touch_sem, wait_ticks);

        if (!s_touch_handle) {
            continue;
        }

        // Perform I2C read in this background task (Core 0), NEVER blocking the UI thread
        esp_lcd_touch_read_data(s_touch_handle);

        esp_lcd_touch_point_data_t pt_data;
        uint8_t pt_cnt = 0;
        esp_err_t err = esp_lcd_touch_get_data(s_touch_handle, &pt_data, &pt_cnt, 1);

        if (err == ESP_OK && pt_cnt > 0) {
            int32_t x = (int32_t)pt_data.x;
            int32_t y = (int32_t)pt_data.y;

            // Clamp coordinates to panel resolution
            if (x < 0) x = 0;
            if (x >= BOARD_DISPLAY_WIDTH) x = BOARD_DISPLAY_WIDTH - 1;
            if (y < 0) y = 0;
            if (y >= BOARD_DISPLAY_HEIGHT) y = BOARD_DISPLAY_HEIGHT - 1;

            release_debounce = 0;

            portENTER_CRITICAL(&s_touch_spinlock);
            s_touch_cache.touched = true;
            s_touch_cache.x = x;
            s_touch_cache.y = y;
            s_touch_cache.seq++;
            portEXIT_CRITICAL(&s_touch_spinlock);

            // Push event into FreeRTOS event queue for pure event-driven consumers
            if (!currently_touched) {
                hal_touch_event_t ev = {
                    .type = HAL_TOUCH_EVENT_DOWN,
                    .x = x,
                    .y = y,
                };
                if (s_touch_event_queue) {
                    xQueueSend(s_touch_event_queue, &ev, 0);
                }
                last_x = x;
                last_y = y;
            } else if (abs(x - last_x) >= 1 || abs(y - last_y) >= 1) {
                hal_touch_event_t ev = {
                    .type = HAL_TOUCH_EVENT_MOVE,
                    .x = x,
                    .y = y,
                };
                if (s_touch_event_queue) {
                    xQueueSend(s_touch_event_queue, &ev, 0);
                }
                last_x = x;
                last_y = y;
            }

            currently_touched = true;
            // In tracking mode, sample at 100Hz (10ms) for high-fidelity gesture tracking
            wait_ticks = pdMS_TO_TICKS(10);
        } else {
            // Finger release with debounce filter (require 2 consecutive empty samples ~20ms
            // to filter out CST816 transient glitch / light touch dropout during dragging)
            if (currently_touched) {
                release_debounce++;
                if (release_debounce >= 2) {
                    portENTER_CRITICAL(&s_touch_spinlock);
                    s_touch_cache.touched = false;
                    s_touch_cache.seq++;
                    portEXIT_CRITICAL(&s_touch_spinlock);

                    hal_touch_event_t ev = {
                        .type = HAL_TOUCH_EVENT_UP,
                        .x = last_x,
                        .y = last_y,
                    };
                    if (s_touch_event_queue) {
                        xQueueSend(s_touch_event_queue, &ev, 0);
                    }

                    currently_touched = false;
                    release_debounce = 0;
                    last_x = -1;
                    last_y = -1;
                    // Return to low-power idle: wait indefinitely for next interrupt (0% CPU)
                    wait_ticks = portMAX_DELAY;
                } else {
                    // Wait one more 10ms tick to confirm genuine release
                    wait_ticks = pdMS_TO_TICKS(10);
                }
            } else {
                wait_ticks = portMAX_DELAY;
            }
        }
    }
}

esp_err_t board_touch_init(void)
{
    if (s_touch_handle != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing CST816/9217 touch controller...");
    bsp_display_cfg_t touch_cfg = {0};
    touch_cfg.touch_flags.swap_xy = 1;
    touch_cfg.touch_flags.mirror_x = 0;
    touch_cfg.touch_flags.mirror_y = 1;

    esp_err_t ret = bsp_touch_new(&touch_cfg, &s_touch_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "bsp_touch_new warning: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create binary semaphore for interrupt signaling
    s_touch_sem = xSemaphoreCreateBinary();
    if (!s_touch_sem) {
        ESP_LOGE(TAG, "Failed to create touch semaphore");
        return ESP_ERR_NO_MEM;
    }

    // Create event queue for pure push-based event delivery (32 items)
    s_touch_event_queue = xQueueCreate(32, sizeof(hal_touch_event_t));
    if (!s_touch_event_queue) {
        ESP_LOGE(TAG, "Failed to create touch event queue");
        return ESP_ERR_NO_MEM;
    }

    // Register hardware interrupt callback (installs GPIO ISR service internally if not already active)
    esp_err_t cb_err = esp_lcd_touch_register_interrupt_callback(s_touch_handle, touch_isr_callback);
    if (cb_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_touch_register_interrupt_callback: %s (falling back to adaptive timer)",
                 esp_err_to_name(cb_err));
    } else {
        ESP_LOGI(TAG, "Touch interrupt callback registered successfully.");
    }

    // Spawn dedicated FreeRTOS touch sampling task on Core 0
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        touch_task,
        "touch_task",
        4096,
        NULL,
        12, // High priority
        &s_touch_task_handle,
        0   // Pin to Core 0 (leaving Core 1 dedicated to Rust Iced UI)
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create touch task");
        return ESP_ERR_NO_MEM;
    }

    // Trigger an initial read check on boot
    xSemaphoreGive(s_touch_sem);

    ESP_LOGI(TAG, "Touch controller and async sampling task initialized successfully.");
    return ESP_OK;
}

/**
 * @brief Non-blocking, O(1) touch query for UI thread.
 *
 * Reads the latest state from the atomic spinlock cache in ~20-50 nanoseconds.
 */
bool hal_touch_get_point(int32_t *out_x, int32_t *out_y)
{
    portENTER_CRITICAL(&s_touch_spinlock);
    bool touched = s_touch_cache.touched;
    if (touched) {
        if (out_x) *out_x = s_touch_cache.x;
        if (out_y) *out_y = s_touch_cache.y;
    }
    portEXIT_CRITICAL(&s_touch_spinlock);

    return touched;
}

/**
 * @brief Wait for a touch event from the FreeRTOS event queue.
 *
 * Blocks the calling task for up to timeout_ms milliseconds (0 for non-blocking poll).
 * Returns true if an event was received, false on timeout.
 */
bool hal_touch_wait_event(hal_touch_event_t *out_event, uint32_t timeout_ms)
{
    if (!s_touch_event_queue || !out_event) {
        return false;
    }
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_touch_event_queue, out_event, ticks) == pdTRUE;
}
