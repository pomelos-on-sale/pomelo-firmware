#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "board_hal.h"
#include "board_pins.h"

static const char *TAG = "board_storage";

#if BOARD_HAS_SD_CARD

#include "esp_vfs_fat.h"
#include "vfs_fat_internal.h"
#include "diskio_sdmmc.h"
#include "diskio_impl.h"
#include "ff.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#define BOARD_SD_MOUNT_POINT "/sdcard"

static hal_sd_status_t s_sd_status = HAL_SD_STATUS_NOT_INSERTED;
static hal_sd_card_info_t s_card_info = {0};
static sdmmc_card_t *s_card = NULL;
static TaskHandle_t s_monitor_task = NULL;
static bool s_host_inited = false;
static SemaphoreHandle_t s_sd_mutex = NULL;

static const sdmmc_host_t s_host = SDMMC_HOST_DEFAULT();
static const sdmmc_slot_config_t s_slot_config = {
    .clk = BOARD_PIN_SD_CLK,
    .cmd = BOARD_PIN_SD_CMD,
    .d0 = BOARD_PIN_SD_D0,
    .d1 = GPIO_NUM_NC,
    .d2 = GPIO_NUM_NC,
    .d3 = GPIO_NUM_NC,
    .d4 = GPIO_NUM_NC,
    .d5 = GPIO_NUM_NC,
    .d6 = GPIO_NUM_NC,
    .d7 = GPIO_NUM_NC,
    .cd = SDMMC_SLOT_NO_CD,
    .wp = SDMMC_SLOT_NO_WP,
    .width = 1,
    .flags = 0,
};

static void notify_sd_status_changed(hal_sd_status_t status)
{
    hal_event_t ev = {
        .type = HAL_EVENT_SD,
        .data.sd = (int32_t)status,
    };
    hal_event_send(&ev);
}

static esp_err_t ensure_host_inited(void)
{
    if (s_host_inited) {
        return ESP_OK;
    }
    esp_err_t err = (*s_host.init)();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc host init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = sdmmc_host_init_slot(s_host.slot, &s_slot_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc host init slot failed: %s", esp_err_to_name(err));
        return err;
    }
    s_host_inited = true;
    return ESP_OK;
}

static esp_err_t hal_storage_sd_mount_internal(void)
{
    if (s_sd_status == HAL_SD_STATUS_MOUNTED) {
        return ESP_OK;
    }

    if (ensure_host_inited() != ESP_OK) {
        return ESP_FAIL;
    }

    if (s_card == NULL) {
        s_card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
        if (s_card == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t probe_ret = sdmmc_card_init(&s_host, s_card);
    if (probe_ret != ESP_OK) {
        if (s_sd_status != HAL_SD_STATUS_NOT_INSERTED) {
            s_sd_status = HAL_SD_STATUS_NOT_INSERTED;
            memset(&s_card_info, 0, sizeof(s_card_info));
            notify_sd_status_changed(s_sd_status);
        }
        return probe_ret;
    }

    strncpy(s_card_info.name, s_card->cid.name, sizeof(s_card_info.name) - 1);
    s_card_info.total_bytes = ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size;
    s_card_info.is_formatted = false;

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    esp_log_level_set("diskio_sdmmc", ESP_LOG_NONE);
    esp_err_t mount_ret = esp_vfs_fat_mount_initialized(s_card, BOARD_SD_MOUNT_POINT, &mount_config);
    esp_log_level_set("diskio_sdmmc", ESP_LOG_ERROR);
    if (mount_ret == ESP_OK) {
        s_sd_status = HAL_SD_STATUS_MOUNTED;
        s_card_info.is_formatted = true;
        ESP_LOGI(TAG, "SD card (%s, %llu MB) mounted at %s",
                 s_card_info.name,
                 (unsigned long long)(s_card_info.total_bytes / (1024 * 1024)),
                 BOARD_SD_MOUNT_POINT);
    } else {
        BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
        if (pdrv != 0xFF) {
            char drv[3] = {(char)('0' + pdrv), ':', 0};
            f_mount(0, drv, 0);
            ff_diskio_unregister(pdrv);
        }
        esp_vfs_fat_unregister_path(BOARD_SD_MOUNT_POINT);

        s_sd_status = HAL_SD_STATUS_UNFORMATTED;
        s_card_info.is_formatted = false;
        ESP_LOGW(TAG, "SD card (%s, %llu MB) detected but unformatted (err=%s, 0x%x)",
                 s_card_info.name,
                 (unsigned long long)(s_card_info.total_bytes / (1024 * 1024)),
                 esp_err_to_name(mount_ret), mount_ret);
    }

    notify_sd_status_changed(s_sd_status);
    return mount_ret;
}

static esp_err_t hal_storage_sd_unmount_internal(void)
{
    if (s_sd_status == HAL_SD_STATUS_MOUNTED && s_card != NULL) {
        esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_host_inited = false;
    } else if (s_sd_status == HAL_SD_STATUS_UNFORMATTED && s_card != NULL) {
        BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
        if (pdrv != 0xFF) {
            char drv[3] = {(char)('0' + pdrv), ':', 0};
            f_mount(0, drv, 0);
            ff_diskio_unregister(pdrv);
        }
        esp_vfs_fat_unregister_path(BOARD_SD_MOUNT_POINT);
        free(s_card);
        s_card = NULL;
        if (s_host.deinit_p) {
            (*s_host.deinit_p)(s_host.slot);
        } else if (s_host.deinit) {
            (*s_host.deinit)();
        }
        s_host_inited = false;
    }

    s_sd_status = HAL_SD_STATUS_NOT_INSERTED;
    memset(&s_card_info, 0, sizeof(s_card_info));
    notify_sd_status_changed(s_sd_status);
    return ESP_OK;
}

static esp_err_t hal_storage_sd_format_internal(void)
{
    ESP_LOGI(TAG, "Formatting SD card at %s to FAT32...", BOARD_SD_MOUNT_POINT);

    if (s_sd_status != HAL_SD_STATUS_MOUNTED && s_sd_status != HAL_SD_STATUS_UNFORMATTED) {
        ESP_LOGW(TAG, "Cannot format: no SD card detected in slot");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_card == NULL) {
        ESP_LOGE(TAG, "Cannot format: s_card handle is null");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sd_status == HAL_SD_STATUS_MOUNTED) {
        esp_vfs_fat_sdcard_unmount(BOARD_SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_host_inited = false;
        if (ensure_host_inited() != ESP_OK) {
            return ESP_FAIL;
        }
        s_card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
        if (!s_card || sdmmc_card_init(&s_host, s_card) != ESP_OK) {
            ESP_LOGE(TAG, "Re-probe card before format failed");
            s_sd_status = HAL_SD_STATUS_NOT_INSERTED;
            notify_sd_status_changed(s_sd_status);
            return ESP_FAIL;
        }
    } else if (s_sd_status == HAL_SD_STATUS_UNFORMATTED) {
        BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
        if (pdrv != 0xFF) {
            char drv[3] = {(char)('0' + pdrv), ':', 0};
            f_mount(0, drv, 0);
            ff_diskio_unregister(pdrv);
        }
        esp_vfs_fat_unregister_path(BOARD_SD_MOUNT_POINT);
    }

    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) {
        ESP_LOGE(TAG, "ff_diskio_get_drive failed");
        return ESP_ERR_NO_MEM;
    }
    ff_diskio_register_sdmmc(pdrv, s_card);

    char drv[3] = {(char)('0' + pdrv), ':', 0};
    FATFS fs;
    FRESULT fr = f_mount(&fs, drv, 0);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_mount for format failed: %d", fr);
        ff_diskio_unregister(pdrv);
        return ESP_FAIL;
    }

    MKFS_PARM opt = {
        .fmt = FM_FAT32,
        .n_fat = 1,
        .align = 0,
        .n_root = 0,
        .au_size = 16 * 1024,
    };
    uint8_t work_buf[FF_MAX_SS];
    ESP_LOGI(TAG, "Executing f_mkfs (FAT32, 16KB cluster)...");
    fr = f_mkfs(drv, &opt, work_buf, sizeof(work_buf));
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_mkfs failed: %d", fr);
        f_mount(0, drv, 0);
        ff_diskio_unregister(pdrv);
        s_sd_status = HAL_SD_STATUS_UNFORMATTED;
        notify_sd_status_changed(s_sd_status);
        return ESP_FAIL;
    }

    f_mount(0, drv, 0);
    ff_diskio_unregister(pdrv);

    ESP_LOGI(TAG, "Mounting formatted SD card to %s...", BOARD_SD_MOUNT_POINT);
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t mount_ret = esp_vfs_fat_mount_initialized(s_card, BOARD_SD_MOUNT_POINT, &mount_cfg);
    if (mount_ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount after format failed: %s (0x%x)", esp_err_to_name(mount_ret), mount_ret);
        BYTE cur_pdrv = ff_diskio_get_pdrv_card(s_card);
        if (cur_pdrv != 0xFF) {
            char cur_drv[3] = {(char)('0' + cur_pdrv), ':', 0};
            f_mount(0, cur_drv, 0);
            ff_diskio_unregister(cur_pdrv);
        }
        esp_vfs_fat_unregister_path(BOARD_SD_MOUNT_POINT);
        s_sd_status = HAL_SD_STATUS_UNFORMATTED;
        notify_sd_status_changed(s_sd_status);
        return mount_ret;
    }

    s_sd_status = HAL_SD_STATUS_MOUNTED;
    s_card_info.is_formatted = true;
    ESP_LOGI(TAG, "SD card (%s, %llu MB) successfully formatted and mounted at %s",
             s_card_info.name,
             (unsigned long long)(s_card_info.total_bytes / (1024 * 1024)),
             BOARD_SD_MOUNT_POINT);
    notify_sd_status_changed(s_sd_status);
    return ESP_OK;
}

static void sd_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Storage auto-detect monitor task started on Core 0");

    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_NONE);

    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    ensure_host_inited();
    hal_storage_sd_mount_internal();
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
        if (s_sd_status == HAL_SD_STATUS_MOUNTED || s_sd_status == HAL_SD_STATUS_UNFORMATTED) {
            if (s_card != NULL) {
                esp_err_t err = sdmmc_get_status(s_card);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "SD card disconnected or removed (%s), unmounting...", esp_err_to_name(err));
                    hal_storage_sd_unmount_internal();
                }
            }
        } else if (s_sd_status == HAL_SD_STATUS_NOT_INSERTED) {
            hal_storage_sd_mount_internal();
        }
        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    }
}

#endif // BOARD_HAS_SD_CARD

/* ---------------------------------------------------------------------------
 * Public MicroSD Storage API Implementation
 * ------------------------------------------------------------------------- */

hal_sd_status_t hal_storage_sd_get_status(void)
{
#if BOARD_HAS_SD_CARD
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    hal_sd_status_t status = s_sd_status;
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return status;
#else
    return HAL_SD_STATUS_NOT_INSERTED;
#endif
}

esp_err_t hal_storage_sd_get_info(hal_sd_card_info_t *out_info)
{
#if BOARD_HAS_SD_CARD
    if (!out_info) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    if (s_sd_status == HAL_SD_STATUS_NOT_INSERTED) {
        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(out_info, &s_card_info, sizeof(hal_sd_card_info_t));
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t hal_storage_sd_mount(void)
{
#if BOARD_HAS_SD_CARD
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    esp_err_t ret = hal_storage_sd_mount_internal();
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t hal_storage_sd_unmount(void)
{
#if BOARD_HAS_SD_CARD
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    esp_err_t ret = hal_storage_sd_unmount_internal();
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t hal_storage_sd_format(void)
{
#if BOARD_HAS_SD_CARD
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    esp_err_t ret = hal_storage_sd_format_internal();
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t hal_storage_sd_get_usage(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
#if BOARD_HAS_SD_CARD
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    if (s_sd_status != HAL_SD_STATUS_MOUNTED) {
        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_vfs_fat_info(BOARD_SD_MOUNT_POINT, out_total_bytes, out_free_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_info failed for %s: %s (0x%x)",
                 BOARD_SD_MOUNT_POINT, esp_err_to_name(ret), ret);
    } else {
        ESP_LOGD(TAG, "SD usage queried: total=%llu MB, free=%llu MB",
                 out_total_bytes ? (unsigned long long)(*out_total_bytes / (1024 * 1024)) : 0,
                 out_free_bytes ? (unsigned long long)(*out_free_bytes / (1024 * 1024)) : 0);
    }
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ret;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t hal_storage_sd_init(void)
{
#if BOARD_HAS_SD_CARD
    if (s_monitor_task != NULL) {
        return ESP_OK;
    }

    if (s_sd_mutex == NULL) {
        s_sd_mutex = xSemaphoreCreateMutex();
        if (s_sd_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t res = xTaskCreatePinnedToCore(
        sd_monitor_task,
        "sd_mon",
        4096,
        NULL,
        1,
        &s_monitor_task,
        0
    );

    return (res == pdPASS) ? ESP_OK : ESP_FAIL;
#else
    ESP_LOGI(TAG, "MicroSD card slot bypassed: SD detection disabled");
    return ESP_OK;
#endif
}

/* ---------------------------------------------------------------------------
 * Flash NVS Key-Value Storage API Implementation
 * ------------------------------------------------------------------------- */

esp_err_t hal_nvs_get_blob(const char *part, const char *ns, const char *key, void *out_buf, size_t *inout_len)
{
    if (!ns || !key || !inout_len) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = (part && part[0]) ? part : "nvs_app";
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(p, ns, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(handle, key, out_buf, inout_len);
    nvs_close(handle);
    return err;
}

esp_err_t hal_nvs_set_blob(const char *part, const char *ns, const char *key, const void *data, size_t len)
{
    if (!ns || !key || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = (part && part[0]) ? part : "nvs_app";
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(p, ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t hal_nvs_erase_key(const char *part, const char *ns, const char *key)
{
    if (!ns || !key) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = (part && part[0]) ? part : "nvs_app";
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(p, ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(handle, key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t hal_nvs_dump_all(const char *part, hal_nvs_dump_cb cb, void *ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = (part && part[0]) ? part : "nvs_app";
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(p, NULL, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        size_t len = 0;
        if (hal_nvs_get_blob(p, info.namespace_name, info.key, NULL, &len) == ESP_OK && len > 0) {
            uint8_t *buf = (uint8_t *)malloc(len);
            if (buf) {
                if (hal_nvs_get_blob(p, info.namespace_name, info.key, buf, &len) == ESP_OK) {
                    cb(info.namespace_name, info.key, buf, len, ctx);
                }
                free(buf);
            }
        }
        err = nvs_entry_next(&it);
    }
    if (it != NULL) {
        nvs_release_iterator(it);
    }
    return ESP_OK;
}
