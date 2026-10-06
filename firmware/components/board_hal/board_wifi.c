// =============================================================================
// Wi-Fi Station HAL (ESP-IDF esp_wifi)
// -----------------------------------------------------------------------------
// Thread-safe, non-blocking C surface for the Rust `WifiBackend`:
//   * All long operations are started and then polled (scan state / status).
//   * A FreeRTOS mutex guards a cached status snapshot which the WIFI_EVENT /
//     IP_EVENT handlers keep up to date, so Rust getters never block.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "board_hal.h"

static const char *TAG = "board_wifi";

#define MAX_AP_RECORDS 20

static bool             s_inited = false;
static bool             s_enabled = false;
static esp_netif_t     *s_sta_netif = NULL;
static SemaphoreHandle_t s_lock = NULL;

// ---- Cached state (guarded by s_lock) ---------------------------------------
static int32_t        s_scan_state = HAL_WIFI_SCAN_IDLE;
static hal_wifi_ap_t  s_ap_records[MAX_AP_RECORDS];
static uint16_t       s_ap_count = 0;
static int32_t        s_conn_state = HAL_WIFI_STATE_DISCONNECTED;
static char           s_ssid[HAL_WIFI_SSID_MAX_LEN] = {0};
static char           s_ip[HAL_WIFI_IP_MAX_LEN] = {0};
static char           s_netmask[HAL_WIFI_IP_MAX_LEN] = {0};
static char           s_gateway[HAL_WIFI_IP_MAX_LEN] = {0};
static int8_t         s_rssi = 0;

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

// =============================================================================
// Event handlers
// =============================================================================

static void on_scan_done(void)
{
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK) {
        num = 0;
    }

    uint16_t capacity = (num > MAX_AP_RECORDS) ? MAX_AP_RECORDS : num;
    wifi_ap_record_t *recs = NULL;
    uint16_t got = 0;

    if (capacity > 0) {
        recs = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * capacity);
        if (recs && esp_wifi_scan_get_ap_records(&capacity, recs) == ESP_OK) {
            got = capacity;
        }
    }

    lock();
    for (uint16_t i = 0; i < got; i++) {
        memset(s_ap_records[i].ssid, 0, HAL_WIFI_SSID_MAX_LEN);
        strncpy(s_ap_records[i].ssid, (const char *)recs[i].ssid, HAL_WIFI_SSID_MAX_LEN - 1);
        s_ap_records[i].rssi = recs[i].rssi;
        s_ap_records[i].channel = recs[i].primary;
        s_ap_records[i].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
    }
    s_ap_count = got;
    s_scan_state = HAL_WIFI_SCAN_DONE;
    unlock();

    if (recs) {
        free(recs);
    }
    ESP_LOGI(TAG, "Scan complete: %u AP(s) found", (unsigned)got);
}

/// The name of the common disconnect reasons, by number.
///
/// By number and not by `WIFI_REASON_*`: those macros have been renamed between IDF versions, and a
/// number is never wrong. This is a convenience over the log, not a source of truth.
static const char *wifi_reason_name(uint8_t reason)
{
    switch (reason) {
    case 1:   return "unspecified";
    case 2:   return "auth expire";
    case 4:   return "assoc expire";
    case 15:  return "4-way handshake timeout";
    case 201: return "no AP found";
    case 202: return "auth fail";
    case 203: return "assoc fail";
    case 204: return "handshake timeout";
    case 205: return "connection fail";
    default:  return "see the IDF docs";
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_SCAN_DONE:
            on_scan_done();
            {
                hal_event_t ev = {
                    .type = HAL_EVENT_WIFI,
                    .data.wifi = { .type = HAL_WIFI_EVENT_SCAN_DONE, .status = 0 },
                };
                hal_event_send(&ev);
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            lock();
            s_conn_state = HAL_WIFI_STATE_CONNECTING;
            unlock();
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            lock();
            s_conn_state = HAL_WIFI_STATE_DISCONNECTED;
            s_ssid[0] = '\0';
            s_ip[0] = '\0';
            s_netmask[0] = '\0';
            s_gateway[0] = '\0';
            s_rssi = 0;
            unlock();
            // The reason is the whole diagnosis, and it was being thrown away. 15 and 204 are a failed
            // four-way handshake — the password did not work; 201 is a network that is no longer
            // there; 2 and 202 are the AP refusing the authentication outright. Without it, one
            // disconnect reads exactly like another: wrong password and out of range are the same
            // line — and the app's own `[wifi]` lines cannot tell them apart either.
            ESP_LOGW(TAG, "Wi-Fi disconnected: reason %u (%s)",
                     (unsigned)((wifi_event_sta_disconnected_t *)data)->reason,
                     wifi_reason_name(((wifi_event_sta_disconnected_t *)data)->reason));
            {
                hal_event_t ev = {
                    .type = HAL_EVENT_WIFI,
                    .data.wifi = { .type = HAL_WIFI_EVENT_DISCONNECTED, .status = 0 },
                };
                hal_event_send(&ev);
            }
            break;

        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;

        char ip[HAL_WIFI_IP_MAX_LEN];
        char mask[HAL_WIFI_IP_MAX_LEN];
        char gw[HAL_WIFI_IP_MAX_LEN];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(mask, sizeof(mask), IPSTR, IP2STR(&ev->ip_info.netmask));
        snprintf(gw, sizeof(gw), IPSTR, IP2STR(&ev->ip_info.gw));

        int8_t rssi = 0;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }

        lock();
        snprintf(s_ip, sizeof(s_ip), "%s", ip);
        snprintf(s_netmask, sizeof(s_netmask), "%s", mask);
        snprintf(s_gateway, sizeof(s_gateway), "%s", gw);
        s_rssi = rssi;
        s_conn_state = HAL_WIFI_STATE_CONNECTED;
        unlock();
        ESP_LOGI(TAG, "Wi-Fi connected: ip=%s rssi=%d", ip, (int)rssi);
        {
            hal_event_t sys_ev = {
                .type = HAL_EVENT_WIFI,
                .data.wifi = { .type = HAL_WIFI_EVENT_CONNECTED, .status = rssi },
            };
            hal_event_send(&sys_ev);
        }
    }
}

// =============================================================================
// Public API
// =============================================================================

esp_err_t hal_wifi_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Wi-Fi needs NVS for calibration / config storage.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_flash_init: %s", esp_err_to_name(ret));
    }

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(ev));
        return ev;
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        return ret;
    }

    (void)esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL);
    (void)esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL);

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode: %s", esp_err_to_name(ret));
        return ret;
    }

    s_inited = true;
    ESP_LOGI(TAG, "Wi-Fi station initialized (radio off)");
    return ESP_OK;
}

esp_err_t hal_wifi_set_enabled(bool on)
{
    if (!s_inited && hal_wifi_init() != ESP_OK) {
        return ESP_FAIL;
    }

    if (on) {
        if (s_enabled) {
            return ESP_OK;
        }
        esp_err_t ret = esp_wifi_start();
        if (ret != ESP_OK && ret != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_start: %s", esp_err_to_name(ret));
            return ESP_FAIL;
        }
        s_enabled = true;
        ESP_LOGI(TAG, "Wi-Fi enabled");
    } else {
        if (!s_enabled) {
            return ESP_OK;
        }
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        s_enabled = false;

        lock();
        s_conn_state = HAL_WIFI_STATE_DISCONNECTED;
        s_scan_state = HAL_WIFI_SCAN_IDLE;
        s_ssid[0] = '\0';
        s_ip[0] = '\0';
        s_netmask[0] = '\0';
        s_gateway[0] = '\0';
        s_rssi = 0;
        s_ap_count = 0;
        unlock();

        ESP_LOGI(TAG, "Wi-Fi disabled");
    }
    return ESP_OK;
}

bool hal_wifi_is_enabled(void)
{
    return s_enabled;
}

esp_err_t hal_wifi_scan_start(void)
{
    if (!s_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    lock();
    s_scan_state = HAL_WIFI_SCAN_SCANNING;
    unlock();

    wifi_scan_config_t scan_cfg = {0};
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false); // false = asynchronous
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start: %s", esp_err_to_name(ret));
        lock();
        s_scan_state = HAL_WIFI_SCAN_ERROR;
        unlock();
        return ret;
    }
    return ESP_OK;
}

int32_t hal_wifi_scan_get_state(void)
{
    lock();
    int32_t state = s_scan_state;
    unlock();
    return state;
}

esp_err_t hal_wifi_scan_get_results(hal_wifi_ap_t *records, uint16_t max_count, uint16_t *out_count)
{
    if (!records || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    uint16_t n = (s_ap_count < max_count) ? s_ap_count : max_count;
    if (n > 0) {
        memcpy(records, s_ap_records, sizeof(hal_wifi_ap_t) * n);
    }
    *out_count = n;
    unlock();
    return ESP_OK;
}

esp_err_t hal_wifi_connect(const char *ssid, const char *password)
{
    if (!s_enabled || !ssid) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wc.sta.password, password, sizeof(wc.sta.password) - 1);
    }

    // The threshold is not one number for both cases, and with a password `WIFI_AUTH_OPEN` is a trap.
    //
    // `libnet80211` raises `OPEN` to `WPA2_PSK` by itself when the password is long enough for WPA2
    // — it logs "Password length matches WPA2 standards, authmode threshold changes from OPEN to
    // WPA2" — and an access point that advertises `WPA_PSK` is then below the threshold and gets
    // filtered out *before* association. The connection fails with reason 211,
    // `NO_AP_FOUND_IN_AUTHMODE_THRESHOLD`, and the password is never offered to anybody: the log
    // blames the network for a password that never left the board.
    //
    // `WPA_PSK` is the floor that means "encrypted": WPA, WPA2, the mixed mode and WPA3 all compare
    // above it. Without a password the floor has to stay `OPEN`, or an open network would be filtered
    // out by our own threshold.
    wc.sta.threshold.authmode = wc.sta.password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    // The credentials are the app's now, and this is the line that makes that true rather than a
    // second copy. `WIFI_STORAGE_FLASH` is IDF's default -- board_wifi.c never chose it -- so
    // without this, every attempt writes the SSID and password into `nvs.net80211` whether it
    // works or not, and the driver's opinion of which network this board belongs to sits in flash
    // beside the app's file with nothing to reconcile them. The app writes
    // /internal/AppData/WIFI/wifi.conf instead; see `pomelo-hal`'s `wifi_credentials`.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    lock();
    s_conn_state = HAL_WIFI_STATE_CONNECTING;
    strncpy(s_ssid, ssid, HAL_WIFI_SSID_MAX_LEN - 1);
    s_ssid[HAL_WIFI_SSID_MAX_LEN - 1] = '\0';
    unlock();

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_config: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);
    return ESP_OK;
}

esp_err_t hal_wifi_disconnect(void)
{
    if (!s_inited) {
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_CONNECT) {
        return ret;
    }
    return ESP_OK;
}

esp_err_t hal_wifi_get_status(hal_wifi_status_t *out_status)
{
    if (!out_status) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_status, 0, sizeof(*out_status));
    if (!s_inited) {
        out_status->state = HAL_WIFI_STATE_DISCONNECTED;
        return ESP_OK;
    }

    lock();
    out_status->state = s_conn_state;
    out_status->rssi = s_rssi;
    memcpy(out_status->ssid, s_ssid, HAL_WIFI_SSID_MAX_LEN);
    memcpy(out_status->ip, s_ip, HAL_WIFI_IP_MAX_LEN);
    memcpy(out_status->netmask, s_netmask, HAL_WIFI_IP_MAX_LEN);
    memcpy(out_status->gateway, s_gateway, HAL_WIFI_IP_MAX_LEN);
    unlock();
    return ESP_OK;
}
