#ifndef BOARD_HAL_H
#define BOARD_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Display panel geometry (2.16-inch CO5300 AMOLED)
 * ------------------------------------------------------------------------- */
#define BOARD_DISPLAY_WIDTH   480
#define BOARD_DISPLAY_HEIGHT  480


/**
 * @brief Initialize AMOLED display and CST816 touch controller
 */
esp_err_t board_hal_init(void);

/**
 * @brief Draw RGB565 bitmap to display bounding box [x1, y1) to [x2, y2).
 * Handles Big-Endian byte swap required by CO5300 and DMA chunking.
 *
 * Coordinates snap outward to even pixels (CO5300 requirement).
 */
void hal_display_draw_bitmap(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                            const uint16_t *pixels, int32_t stride);

/**
 * @brief Wait for display vertical synchronization (hardware TE interrupt or 60Hz frame pacing).
 */
void hal_display_wait_vsync(void);

/**
 * @brief Turn display panel power on or off (e.g. sleep/wake).
 */
void hal_display_set_power(bool on);

/**
 * @brief Poll touch point. Returns true if touched, false if released.
 */
bool hal_touch_get_point(int32_t *out_x, int32_t *out_y);

/**
 * @brief Touch event types for interrupt-driven event queues.
 */
typedef enum {
    HAL_TOUCH_EVENT_NONE = 0,
    HAL_TOUCH_EVENT_DOWN,
    HAL_TOUCH_EVENT_MOVE,
    HAL_TOUCH_EVENT_UP,
} hal_touch_event_type_t;

typedef struct {
    hal_touch_event_type_t type;
    int32_t x;
    int32_t y;
} hal_touch_event_t;

/**
 * @brief Wait for a touch event from the FreeRTOS event queue.
 *
 * Blocks the calling task for up to timeout_ms milliseconds (0 for non-blocking poll).
 * Returns true if an event was received, false on timeout.
 */
bool hal_touch_wait_event(hal_touch_event_t *out_event, uint32_t timeout_ms);

/**
 * @brief Button event types for interrupt-driven event queues.
 */
typedef enum {
    HAL_BUTTON_EVENT_NONE = 0,
    HAL_BUTTON_EVENT_BOOT_PRESS,
    HAL_BUTTON_EVENT_BOOT_RELEASE,
    HAL_BUTTON_EVENT_PWR_PRESS,
    HAL_BUTTON_EVENT_PWR_RELEASE,
    HAL_BUTTON_EVENT_SIDE_PRESS,
    HAL_BUTTON_EVENT_SIDE_RELEASE,
} hal_button_event_t;

/* ---------------------------------------------------------------------------
 * Unified Hardware Event Queue
 * ------------------------------------------------------------------------- */

typedef enum {
    HAL_EVENT_NONE = 0,
    HAL_EVENT_BUTTON,
    HAL_EVENT_POWER,
    HAL_EVENT_WIFI,
} hal_event_type_t;

typedef enum {
    HAL_POWER_STATE_CHARGING_STARTED = 1,
    HAL_POWER_STATE_CHARGING_STOPPED,
    HAL_POWER_STATE_BATTERY_UPDATE,
    HAL_POWER_STATE_PEKEY_SHORT,
    HAL_POWER_STATE_PEKEY_LONG,
} hal_power_state_t;

typedef struct {
    hal_power_state_t state;
    int32_t           percent;
    int32_t           voltage_mv;
    bool              is_charging;
} hal_power_event_t;

typedef enum {
    HAL_WIFI_EVENT_CONNECTED = 1,
    HAL_WIFI_EVENT_DISCONNECTED,
    HAL_WIFI_EVENT_SCAN_DONE,
} hal_wifi_event_type_t;

typedef struct {
    hal_wifi_event_type_t type;
    int32_t               status;
} hal_wifi_event_t;

typedef struct {
    hal_event_type_t type;
    union {
        hal_button_event_t button;
        hal_power_event_t  power;
        hal_wifi_event_t   wifi;
    } data;
} hal_event_t;

esp_err_t hal_event_init(void);
bool hal_event_send(const hal_event_t *ev);
bool hal_event_send_from_isr(const hal_event_t *ev);
bool hal_event_wait(hal_event_t *out_event, uint32_t timeout_ms);

/* ---------------------------------------------------------------------------
 * AXP2101 PMIC power management.
 * ------------------------------------------------------------------------- */

/**
 * @brief Initialize AXP2101 PMIC power management.
 */
esp_err_t hal_power_init(void);

/**
 * @brief Get battery percentage (0-100), or -1 if battery is absent / unknown.
 */
int32_t hal_power_get_battery_percent(void);

/**
 * @brief Check if battery is currently being charged.
 */
bool hal_power_is_charging(void);

/**
 * @brief Get battery voltage in millivolts (mV), or 0 if unavailable.
 */
int32_t hal_power_get_battery_voltage_mv(void);

/* ---------------------------------------------------------------------------
 * Wi-Fi station (esp_wifi).
 *
 * Thread-safe and non-blocking: long operations are started and then polled
 * via the scan-state / status getters. All state is cached by the event
 * handlers in board_wifi.c, so the Rust side never blocks on the driver.
 * ------------------------------------------------------------------------- */

#define HAL_WIFI_SCAN_IDLE      0
#define HAL_WIFI_SCAN_SCANNING  1
#define HAL_WIFI_SCAN_DONE      2
#define HAL_WIFI_SCAN_ERROR    -1

#define HAL_WIFI_STATE_DISCONNECTED 0
#define HAL_WIFI_STATE_SCANNING     1
#define HAL_WIFI_STATE_CONNECTING   2
#define HAL_WIFI_STATE_CONNECTED    3

#define HAL_WIFI_SSID_MAX_LEN 33
#define HAL_WIFI_IP_MAX_LEN   16

typedef struct {
    char    ssid[HAL_WIFI_SSID_MAX_LEN];
    int8_t  rssi;
    uint8_t channel;
    bool    secure;
} hal_wifi_ap_t;

typedef struct {
    int32_t state;                              /* HAL_WIFI_STATE_* */
    char    ssid[HAL_WIFI_SSID_MAX_LEN];        /* connected SSID, "" if none */
    char    ip[HAL_WIFI_IP_MAX_LEN];
    char    netmask[HAL_WIFI_IP_MAX_LEN];
    char    gateway[HAL_WIFI_IP_MAX_LEN];
    int8_t  rssi;
} hal_wifi_status_t;

/**
 * @brief Initialize the Wi-Fi station (NVS + netif + event loop + driver).
 *        The radio stays off until hal_wifi_set_enabled(true) is called.
 */
esp_err_t hal_wifi_init(void);

/** @brief Enable or disable the radio. Enabling starts the station. */
esp_err_t hal_wifi_set_enabled(bool on);
bool      hal_wifi_is_enabled(void);

/** @brief Begin an asynchronous scan; poll hal_wifi_scan_get_state(). */
esp_err_t hal_wifi_scan_start(void);
int32_t   hal_wifi_scan_get_state(void);

/**
 * @brief Copy up to max_count discovered APs into records.
 * @param[out] out_count  Number of records actually written.
 */
esp_err_t hal_wifi_scan_get_results(hal_wifi_ap_t *records, uint16_t max_count, uint16_t *out_count);

/** @brief Connect to an AP (password "" for open networks). Non-blocking. */
esp_err_t hal_wifi_connect(const char *ssid, const char *password);
esp_err_t hal_wifi_disconnect(void);

/** @brief Snapshot the current connection status. */
esp_err_t hal_wifi_get_status(hal_wifi_status_t *out_status);

/* ---------------------------------------------------------------------------
 * ES8311 audio codec and I2S speaker interface (Audio Sink).
 * ------------------------------------------------------------------------- */

/**
 * @brief Initialize ES8311 audio codec and I2S speaker interface
 */
esp_err_t hal_audio_init(void);

/**
 * @brief Open audio codec with specified sample rate, channels, and bits per sample
 */
esp_err_t hal_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);

/**
 * @brief Write PCM audio samples to speaker codec (I2S DMA)
 */
esp_err_t hal_audio_write(const void *data, uint32_t len);

/**
 * @brief Drain I2S DMA pipeline and ensure pending samples are played
 */
esp_err_t hal_audio_drain(void);

/**
 * @brief Set speaker output volume (0 - 100)
 */
esp_err_t hal_audio_set_volume(uint8_t volume);

/**
 * @brief Close audio codec
 */
esp_err_t hal_audio_close(void);

/* ---------------------------------------------------------------------------
 * PCF85063A Real-Time Clock (RTC).
 * ------------------------------------------------------------------------- */

/**
 * @brief Initialize PCF85063A RTC on the shared I2C bus and restore POSIX system clock if valid.
 */
esp_err_t hal_rtc_init(void);


#ifdef __cplusplus
}
#endif

#endif // BOARD_HAL_H
