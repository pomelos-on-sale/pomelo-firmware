#include "board_hal_internal.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include <sys/stat.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static const char *TAG = "board_hal";

#ifndef PATH_MAX
#define PATH_MAX 512
#endif

// ESP-IDF VFS does not support symlinks, so newlib omits lstat and realpath.
// - Rust std::fs::DirEntry::metadata() calls lstat on Unix targets.
// - Rust std::fs::canonicalize() (used by iced -> cosmic-text -> fontdb) calls realpath.
int lstat(const char *path, struct stat *st)
{
    return stat(path, st);
}

char *realpath(const char *path, char *resolved_path)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return NULL;
    }

    if (!resolved_path) {
        resolved_path = malloc(PATH_MAX);
        if (!resolved_path) {
            errno = ENOMEM;
            return NULL;
        }
    }

    strncpy(resolved_path, path, PATH_MAX - 1);
    resolved_path[PATH_MAX - 1] = '\0';
    return resolved_path;
}

static i2c_master_bus_handle_t s_i2c_master_bus = NULL;

i2c_master_bus_handle_t hal_i2c_get_master_bus(void)
{
    if (s_i2c_master_bus != NULL) {
        return s_i2c_master_bus;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_PIN_I2C_NUM,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_master_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create shared I2C master bus: %s", esp_err_to_name(ret));
        return NULL;
    }
    ESP_LOGI(TAG, "Shared I2C master bus initialized (SDA: GPIO%d, SCL: GPIO%d, Port: %d)",
             BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL, BOARD_PIN_I2C_NUM);
    return s_i2c_master_bus;
}

static void board_bg_worker_task(void *arg)
{
    ESP_LOGI(TAG, "Background worker task started on Core %d", xPortGetCoreID());

    // 1. Initialize CST816/9217 touch controller (~80ms I2C reset/probe)
    esp_err_t ret = board_touch_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "board_touch_init warning: %s", esp_err_to_name(ret));
    }

    // 2. Initialize background SD card auto-detection and monitor on Core 0
    ret = hal_storage_sd_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hal_storage_sd_init warning: %s", esp_err_to_name(ret));
    }

    // 3. Initialize HTTP / HTTPS background client on Core 0
    ret = hal_http_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hal_http_init warning: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Background worker task completed peripheral initialization.");
    vTaskDelete(NULL);
}

esp_err_t board_hal_init(void)
{
    // 0. Initialize Flash NVS partitions (default nvs and dedicated nvs_app)
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nvs_err = nvs_flash_init_partition("nvs_app");
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition("nvs_app");
        nvs_flash_init_partition("nvs_app");
    }

    // 0.1. Initialize TCP/IP stack (LwIP) and default event loop for networking
    esp_netif_init();
    esp_event_loop_create_default();

    // 0.5. Initialize unified hardware event queue
    hal_event_init();

    // 0.6. Initialize global GPIO ISR service for buttons and peripherals
    gpio_install_isr_service(0);

    // 0.7. Initialize shared I2C master bus for PMIC, RTC, and Touch
    hal_i2c_get_master_bus();

    // 1. Initialize power management
    esp_err_t ret = hal_power_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hal_power_init warning: %s", esp_err_to_name(ret));
    }

    // 1.5. Initialize RTC and restore system clock
    ret = hal_rtc_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hal_rtc_init warning: %s", esp_err_to_name(ret));
    }

    // 2. Initialize display (QSPI + DMA ping-pong buffers, non-blocking charge pump trigger)
    ret = board_display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "board_display_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Initialize physical button GPIOs (instant, 0ms)
    ret = board_button_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "board_button_init warning: %s", esp_err_to_name(ret));
    }

    // 4. Spawn Background Worker Task on Core 0 for non-visual peripherals (touch, SD, HTTP)
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        board_bg_worker_task,
        "board_bg_worker",
        4096,
        NULL,
        5,  // Normal priority
        NULL,
        0   // Pin to Core 0 (leaving Core 1 dedicated to Rust Iced UI)
    );
    if (task_ret != pdPASS) {
        ESP_LOGW(TAG, "Could not spawn bg_worker, falling back to sync init");
        board_touch_init();
        hal_storage_sd_init();
        hal_http_init();
    }

    return ESP_OK;
}
