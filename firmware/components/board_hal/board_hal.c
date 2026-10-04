#include "board_hal_internal.h"
#include "esp_log.h"
#include <sys/stat.h>

static const char *TAG = "board_hal";

// ESP-IDF VFS does not support symlinks, so newlib omits lstat.
// Rust std::fs::DirEntry::metadata() calls lstat on Unix targets.
int lstat(const char *path, struct stat *st)
{
    return stat(path, st);
}

esp_err_t board_hal_init(void)
{
    // 0. Initialize unified hardware event queue
    hal_event_init();

    // 1. Initialize PMIC power management first to ensure power hold and display rail
    ESP_LOGI(TAG, "Initializing AXP2101 Power Management IC...");
    esp_err_t ret = hal_power_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hal_power_init warning: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "AXP2101 PMIC successfully configured.");
    }

    // 1.5. Initialize PCF85063A hardware RTC and restore system clock
    ESP_LOGI(TAG, "Initializing PCF85063A Real-Time Clock...");
    ret = hal_rtc_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "hal_rtc_init warning: %s", esp_err_to_name(ret));
    }

    // 2. Initialize display (QSPI + DMA ping-pong buffers)
    ret = board_display_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "board_display_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Initialize touch controller
    ret = board_touch_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "board_touch_init warning: %s", esp_err_to_name(ret));
    }

    // 4. Initialize physical button GPIOs
    ret = board_button_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "board_button_init warning: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}
