// =============================================================================
// HTTP / HTTPS Client HAL (ESP-IDF esp_http_client + esp_crt_bundle)
// -----------------------------------------------------------------------------
// Asynchronous, non-blocking HTTP/HTTPS download client running on Core 0:
//   * Dedicated FreeRTOS background task handles TLS handshake and socket I/O.
//   * Mozilla certificate bundle (esp_crt_bundle_attach) provides full CA validation.
//   * Rust UI thread polls atomic snapshot without blocking frame loop.
//   * Automatically rejects HTTPS requests if system clock is not SNTP-synchronized.
// =============================================================================

#include "board_hal.h"
#include "board_hal_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "board_http";

#define HTTP_MAX_URL_LEN 512
#define HTTP_CHUNK_SIZE  2048

typedef enum {
    HTTP_CMD_START = 1,
    HTTP_CMD_CANCEL = 2,
} http_cmd_type_t;

typedef struct {
    http_cmd_type_t type;
    char url[HTTP_MAX_URL_LEN];
} http_cmd_t;

static SemaphoreHandle_t s_lock = NULL;
static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_task_handle = NULL;
static bool s_inited = false;
static volatile bool s_cancel_requested = false;

// Guarded by s_lock
static hal_http_status_snapshot_t s_status = {
    .state = HAL_HTTP_STATE_IDLE,
    .downloaded_bytes = 0,
    .total_bytes = 0,
    .status_code = 0,
    .error_code = 0,
};

static uint8_t *s_result_buf = NULL;
static size_t s_result_size = 0;
static size_t s_result_capacity = 0;

static inline void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static inline void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static void free_result_buf_locked(void)
{
    if (s_result_buf) {
        free(s_result_buf);
        s_result_buf = NULL;
    }
    s_result_size = 0;
    s_result_capacity = 0;
}

static void http_task(void *pvParameters)
{
    http_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd.type != HTTP_CMD_START) {
            continue;
        }

        s_cancel_requested = false;
        ESP_LOGI(TAG, "Starting HTTP GET: %s (free heap: %u, internal: %u, dma: %u)",
                 cmd.url,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

        esp_http_client_config_t config = {
            .url = cmd.url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 15000,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            .skip_cert_common_name_check = false,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to initialize esp_http_client");
            lock();
            s_status.state = HAL_HTTP_STATE_ERROR;
            s_status.error_code = ESP_FAIL;
            unlock();
            hal_event_t ev = {
                .type = HAL_EVENT_HTTP,
                .data.http = { .status_code = 0, .bytes = 0, .error_code = ESP_FAIL },
            };
            hal_event_send(&ev);
            continue;
        }

        int redirect_count = 0;
        const int max_redirects = 5;
        int status_code = 0;
        int content_length = 0;
        bool open_failed = false;

        while (1) {
            if (s_cancel_requested) {
                ESP_LOGI(TAG, "HTTP request cancelled before open/redirect");
                esp_http_client_cleanup(client);
                lock();
                s_status.state = HAL_HTTP_STATE_IDLE;
                unlock();
                open_failed = true;
                break;
            }

            hal_wifi_status_t wifi_status;
            if (hal_wifi_get_status(&wifi_status) != ESP_OK || wifi_status.state != HAL_WIFI_STATE_CONNECTED) {
                ESP_LOGW(TAG, "HTTP request aborted: Wi-Fi is not connected");
                esp_http_client_cleanup(client);
                lock();
                s_status.state = HAL_HTTP_STATE_ERROR;
                s_status.error_code = ESP_ERR_INVALID_STATE;
                unlock();
                hal_event_t ev = {
                    .type = HAL_EVENT_HTTP,
                    .data.http = { .status_code = 0, .bytes = 0, .error_code = ESP_ERR_INVALID_STATE },
                };
                hal_event_send(&ev);
                open_failed = true;
                break;
            }

            esp_err_t err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to open HTTP connection: %s (free heap: %u, internal: %u, dma: %u)",
                         esp_err_to_name(err),
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
                esp_http_client_cleanup(client);
                lock();
                s_status.state = HAL_HTTP_STATE_ERROR;
                s_status.error_code = err;
                unlock();
                hal_event_t ev = {
                    .type = HAL_EVENT_HTTP,
                    .data.http = { .status_code = 0, .bytes = 0, .error_code = err },
                };
                hal_event_send(&ev);
                open_failed = true;
                break;
            }

            content_length = esp_http_client_fetch_headers(client);
            status_code = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP status_code=%d, content_length=%d", status_code, content_length);

            // Follow 3xx redirection if available
            if ((status_code == 301 || status_code == 302 || status_code == 303 ||
                 status_code == 307 || status_code == 308) &&
                redirect_count < max_redirects) {
                esp_err_t redir_err = esp_http_client_set_redirection(client);
                if (redir_err == ESP_OK) {
                    ESP_LOGI(TAG, "Redirecting (status %d, count %d)...", status_code, redirect_count + 1);
                    esp_http_client_close(client);
                    redirect_count++;
                    continue;
                } else {
                    ESP_LOGW(TAG, "Cannot follow redirect: %s, returning redirect response", esp_err_to_name(redir_err));
                }
            }
            break;
        }

        if (open_failed) {
            continue;
        }

        lock();
        s_status.status_code = status_code;
        s_status.total_bytes = (content_length > 0) ? (size_t)content_length : 0;
        unlock();

        // Allocate initial buffer (default 4KB or content_length)
        lock();
        free_result_buf_locked();
        size_t initial_cap = (content_length > 0) ? (size_t)content_length : 4096;
        s_result_buf = (uint8_t *)malloc(initial_cap);
        s_result_capacity = s_result_buf ? initial_cap : 0;
        s_result_size = 0;
        unlock();

        if (!s_result_buf && initial_cap > 0) {
            ESP_LOGE(TAG, "Failed to allocate memory for HTTP response (%u bytes)", (unsigned)initial_cap);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            lock();
            s_status.state = HAL_HTTP_STATE_ERROR;
            s_status.error_code = ESP_ERR_NO_MEM;
            unlock();
            hal_event_t ev = {
                .type = HAL_EVENT_HTTP,
                .data.http = { .status_code = status_code, .bytes = 0, .error_code = ESP_ERR_NO_MEM },
            };
            hal_event_send(&ev);
            continue;
        }

        char chunk[HTTP_CHUNK_SIZE];
        bool failed = false;

        while (1) {
            if (s_cancel_requested) {
                ESP_LOGI(TAG, "HTTP request cancelled by caller");
                failed = true;
                break;
            }

            int read_len = esp_http_client_read(client, chunk, sizeof(chunk));
            if (read_len < 0) {
                ESP_LOGE(TAG, "esp_http_client_read error: %d", read_len);
                failed = true;
                break;
            }
            if (read_len == 0) {
                // End of stream
                break;
            }

            lock();
            if (s_result_size + read_len > s_result_capacity) {
                size_t new_cap = (s_result_capacity * 3) / 2 + read_len + 1024;
                uint8_t *new_buf = (uint8_t *)realloc(s_result_buf, new_cap);
                if (!new_buf) {
                    ESP_LOGE(TAG, "Failed to reallocate response buffer to %u bytes", (unsigned)new_cap);
                    failed = true;
                    unlock();
                    break;
                }
                s_result_buf = new_buf;
                s_result_capacity = new_cap;
            }

            memcpy(s_result_buf + s_result_size, chunk, read_len);
            s_result_size += read_len;
            s_status.downloaded_bytes = s_result_size;
            unlock();
        }

        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        lock();
        hal_http_event_t ev_data = {
            .status_code = s_status.status_code,
            .bytes = s_result_size,
            .error_code = 0,
        };
        if (s_cancel_requested) {
            free_result_buf_locked();
            s_status.state = HAL_HTTP_STATE_IDLE;
            ev_data.error_code = -1;
        } else if (failed) {
            free_result_buf_locked();
            s_status.state = HAL_HTTP_STATE_ERROR;
            s_status.error_code = ESP_FAIL;
            ev_data.error_code = ESP_FAIL;
        } else {
            s_status.state = HAL_HTTP_STATE_DONE;
            s_status.downloaded_bytes = s_result_size;
            ESP_LOGI(TAG, "HTTP GET completed successfully, %u bytes received", (unsigned)s_result_size);
        }
        unlock();

        hal_event_t ev = {
            .type = HAL_EVENT_HTTP,
            .data.http = ev_data,
        };
        hal_event_send(&ev);
    }
}

esp_err_t hal_http_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "Failed to create HTTP mutex");
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(2, sizeof(http_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create HTTP command queue");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        http_task,
        "http_task",
        10240,
        NULL,
        5,
        &s_task_handle,
        0 // Core 0 for background networking
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create http_task");
        vQueueDelete(s_cmd_queue);
        vSemaphoreDelete(s_lock);
        s_cmd_queue = NULL;
        s_lock = NULL;
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "HTTP subsystem initialized successfully on Core 0");
    return ESP_OK;
}

esp_err_t hal_http_start(const char *url)
{
    if (!url || strlen(url) == 0 || strlen(url) >= HTTP_MAX_URL_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Guard: reject HTTP/HTTPS request if Wi-Fi is not connected
    hal_wifi_status_t wifi_status;
    if (hal_wifi_get_status(&wifi_status) != ESP_OK || wifi_status.state != HAL_WIFI_STATE_CONNECTED) {
        ESP_LOGW(TAG, "HTTP request rejected: Wi-Fi is not connected");
        return ESP_ERR_INVALID_STATE;
    }

    // Guard: reject HTTPS if system time is not synchronized
    if (strncmp(url, "https://", 8) == 0 && !hal_sntp_is_synced()) {
        ESP_LOGW(TAG, "HTTPS request rejected: system clock is not synchronized with SNTP");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_inited) {
        esp_err_t init_ret = hal_http_init();
        if (init_ret != ESP_OK) {
            return init_ret;
        }
    }

    lock();
    if (s_status.state == HAL_HTTP_STATE_RUNNING) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    free_result_buf_locked();
    s_status.state = HAL_HTTP_STATE_RUNNING;
    s_status.downloaded_bytes = 0;
    s_status.total_bytes = 0;
    s_status.status_code = 0;
    s_status.error_code = 0;
    unlock();

    http_cmd_t cmd = {
        .type = HTTP_CMD_START,
    };
    strncpy(cmd.url, url, sizeof(cmd.url) - 1);
    cmd.url[sizeof(cmd.url) - 1] = '\0';

    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        lock();
        s_status.state = HAL_HTTP_STATE_IDLE;
        unlock();
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

hal_http_status_snapshot_t hal_http_get_status(void)
{
    lock();
    hal_http_status_snapshot_t snap = s_status;
    unlock();
    return snap;
}

size_t hal_http_get_result_size(void)
{
    lock();
    size_t sz = (s_status.state == HAL_HTTP_STATE_DONE) ? s_result_size : 0;
    unlock();
    return sz;
}

int hal_http_take_result(uint8_t *out_buf, size_t max_len)
{
    lock();
    if (s_status.state != HAL_HTTP_STATE_DONE) {
        unlock();
        return -1;
    }

    size_t to_copy = (s_result_size < max_len) ? s_result_size : max_len;
    if (to_copy > 0 && out_buf && s_result_buf) {
        memcpy(out_buf, s_result_buf, to_copy);
    }

    free_result_buf_locked();
    s_status.state = HAL_HTTP_STATE_IDLE;
    unlock();

    return (int)to_copy;
}

void hal_http_cancel(void)
{
    s_cancel_requested = true;
    lock();
    if (s_status.state == HAL_HTTP_STATE_RUNNING) {
        free_result_buf_locked();
        s_status.state = HAL_HTTP_STATE_IDLE;
    }
    unlock();
}
