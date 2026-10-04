#include "board_hal.h"
#include "pcf85063a.h"
#include "bsp/esp32_s3_touch_amoled_2_16.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/time.h>
#include <time.h>

static const char *TAG = "board_rtc";

static pcf85063a_dev_t s_pcf_dev;
static bool s_rtc_initialized = false;
static SemaphoreHandle_t s_rtc_lock = NULL;

/* Linker wrap prototypes for POSIX time setters */
extern int __real_settimeofday(const struct timeval *tv, const struct timezone *tz);
extern int __real_clock_settime(clockid_t clock_id, const struct timespec *tp);

/**
 * @brief Howard Hinnant algorithm for civil date to days since Unix epoch (1970-01-01).
 * Works for any Gregorian calendar date.
 */
static int days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + (unsigned)(d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

/**
 * @brief Write an epoch timestamp into the PCF85063A hardware RTC chip registers.
 */
static esp_err_t hal_rtc_write_chip_epoch(time_t epoch_sec)
{
    if (!s_rtc_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    struct tm tm;
    if (gmtime_r(&epoch_sec, &tm) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pcf85063a_datetime_t dt = {
        .sec   = (uint8_t)tm.tm_sec,
        .min   = (uint8_t)tm.tm_min,
        .hour  = (uint8_t)tm.tm_hour,
        .day   = (uint8_t)tm.tm_mday,
        .month = (uint8_t)(tm.tm_mon + 1),
        .year  = (uint16_t)(tm.tm_year + 1900),
        .dotw  = (uint8_t)tm.tm_wday,
    };

    if (xSemaphoreTake(s_rtc_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = pcf85063a_set_time_date(&s_pcf_dev, dt);
    xSemaphoreGive(s_rtc_lock);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write PCF85063A time registers: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Hardware RTC chip updated: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
    }
    return ret;
}

/**
 * @brief Read hardware RTC and restore into POSIX system clock if valid (>= 2024).
 */
static esp_err_t hal_rtc_restore_to_system(void)
{
    if (!s_rtc_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_rtc_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    pcf85063a_datetime_t dt;
    esp_err_t ret = pcf85063a_get_time_date(&s_pcf_dev, &dt);
    xSemaphoreGive(s_rtc_lock);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not read RTC to restore system clock: %s", esp_err_to_name(ret));
        return ret;
    }

    // Sanity check: valid RTC year must be >= 2024
    if (dt.year >= 2024 && dt.month >= 1 && dt.month <= 12 && dt.day >= 1 && dt.day <= 31) {
        int days = days_from_civil(dt.year, dt.month, dt.day);
        time_t epoch = (time_t)days * 86400 + dt.hour * 3600 + dt.min * 60 + dt.sec;
        struct timeval tv = {
            .tv_sec = epoch,
            .tv_usec = 0,
        };

        // Directly call __real_settimeofday to bypass the __wrap_settimeofday writeback
        __real_settimeofday(&tv, NULL);

        ESP_LOGI(TAG, "Restored system clock from hardware RTC: %04d-%02d-%02d %02d:%02d:%02d UTC (epoch %lld)",
                 dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec, (long long)epoch);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Hardware RTC time uninitialized (%04d-%02d-%02d %02d:%02d:%02d), skipping system clock restore",
                 dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec);
        return ESP_ERR_INVALID_STATE;
    }
}

/* ---------------------------------------------------------------------------
 * POSIX API Interception (Linker Wrappers)
 * ---------------------------------------------------------------------------
 * Any standard POSIX call to settimeofday() or clock_settime(CLOCK_REALTIME)
 * will invoke these wrappers, automatically persisting time to the hardware RTC.
 * ------------------------------------------------------------------------- */

int __wrap_settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    int ret = __real_settimeofday(tv, tz);
    if (ret == 0 && tv != NULL) {
        if (tv->tv_sec >= 1704067200) { // >= 2024-01-01
            ESP_LOGI(TAG, "POSIX settimeofday called -> automatically syncing to hardware RTC (epoch %lld)",
                     (long long)tv->tv_sec);
            hal_rtc_write_chip_epoch(tv->tv_sec);
        }
    }
    return ret;
}

int __wrap_clock_settime(clockid_t clock_id, const struct timespec *tp)
{
    int ret = __real_clock_settime(clock_id, tp);
    if (ret == 0 && clock_id == CLOCK_REALTIME && tp != NULL) {
        if (tp->tv_sec >= 1704067200) { // >= 2024-01-01
            ESP_LOGI(TAG, "POSIX clock_settime called -> automatically syncing to hardware RTC (epoch %lld)",
                     (long long)tp->tv_sec);
            hal_rtc_write_chip_epoch(tp->tv_sec);
        }
    }
    return ret;
}

/* ---------------------------------------------------------------------------
 * Public HAL RTC API
 * ------------------------------------------------------------------------- */

esp_err_t hal_rtc_init(void)
{
    if (s_rtc_initialized) {
        return ESP_OK;
    }

    if (s_rtc_lock == NULL) {
        s_rtc_lock = xSemaphoreCreateMutex();
        if (s_rtc_lock == NULL) {
            ESP_LOGE(TAG, "Failed to create RTC mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Reuse shared BSP I2C master bus handle (GPIO 14 SCL, GPIO 15 SDA)
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (i2c_bus == NULL) {
        esp_err_t err = bsp_i2c_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize shared BSP I2C bus: %s", esp_err_to_name(err));
            return err;
        }
        i2c_bus = bsp_i2c_get_handle();
    }

    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "No I2C bus handle available for PCF85063A");
        return ESP_FAIL;
    }

    esp_err_t ret = pcf85063a_init(&s_pcf_dev, i2c_bus, PCF85063A_ADDRESS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pcf85063a_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_rtc_initialized = true;
    ESP_LOGI(TAG, "PCF85063A hardware RTC initialized (I2C addr 0x%02X)", PCF85063A_ADDRESS);

    // Attempt restoring system POSIX clock from hardware RTC
    hal_rtc_restore_to_system();

    return ESP_OK;
}
