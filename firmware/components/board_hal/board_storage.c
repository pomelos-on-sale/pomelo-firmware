#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "vfs_fat_internal.h"
#include "diskio_sdmmc.h"
#include "diskio_impl.h"
#include "ff.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "board_hal.h"
#include "bsp/esp32_s3_touch_amoled_2_16.h"

static const char *TAG = "board_storage";

static hal_sd_status_t s_sd_status = HAL_SD_STATUS_NOT_INSERTED;
static hal_sd_card_info_t s_card_info = {0};
static sdmmc_card_t *s_card = NULL;
static TaskHandle_t s_monitor_task = NULL;
static bool s_host_inited = false;
static SemaphoreHandle_t s_sd_mutex = NULL;

static const sdmmc_host_t s_host = SDMMC_HOST_DEFAULT();
static const sdmmc_slot_config_t s_slot_config = {
    .clk = BSP_SD_CLK,
    .cmd = BSP_SD_CMD,
    .d0 = BSP_SD_D0,
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

    hal_sd_status_t prev_status = s_sd_status;

    if (ensure_host_inited() != ESP_OK) {
        s_sd_status = HAL_SD_STATUS_ERROR;
        if (s_sd_status != prev_status) {
            notify_sd_status_changed(s_sd_status);
        }
        return ESP_FAIL;
    }

    if (s_card == NULL) {
        s_card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
        if (s_card == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Stage 1: Hardware probe via SDMMC protocol (CMD0, ACMD41, CID, CSD)
    esp_err_t probe_ret = sdmmc_card_init(&s_host, s_card);
    if (probe_ret != ESP_OK) {
        s_sd_status = (probe_ret == ESP_ERR_TIMEOUT || probe_ret == ESP_ERR_NOT_FOUND)
            ? HAL_SD_STATUS_NOT_INSERTED
            : HAL_SD_STATUS_ERROR;
        memset(&s_card_info, 0, sizeof(s_card_info));
        free(s_card);
        s_card = NULL;
        if (s_host.deinit_p) {
            s_host.deinit_p(s_host.slot);
        }
        s_host_inited = false;
        if (s_sd_status != prev_status) {
            notify_sd_status_changed(s_sd_status);
        }
        return probe_ret;
    }

    // Physical card successfully detected and initialized
    memset(&s_card_info, 0, sizeof(s_card_info));
    snprintf(s_card_info.name, sizeof(s_card_info.name), "%.5s", s_card->cid.name);
    s_card_info.total_bytes = ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size;
    s_card_info.serial_number = s_card->cid.serial;
    s_card_info.is_formatted = false;

    // Stage 2: Mount FAT32 filesystem (without auto-formatting to protect user data)
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    // Temporarily suppress diskio_sdmmc errors during probe: an unformatted card is expected to fail mount
    esp_log_level_set("diskio_sdmmc", ESP_LOG_NONE);
    esp_err_t mount_ret = esp_vfs_fat_mount_initialized(s_card, BSP_SD_MOUNT_POINT, &mount_config);
    esp_log_level_set("diskio_sdmmc", ESP_LOG_ERROR);
    if (mount_ret == ESP_OK) {
        s_sd_status = HAL_SD_STATUS_MOUNTED;
        s_card_info.is_formatted = true;
        ESP_LOGI(TAG, "SD card (%s, %llu MB) mounted at %s",
                 s_card_info.name,
                 (unsigned long long)(s_card_info.total_bytes / (1024 * 1024)),
                 BSP_SD_MOUNT_POINT);
    } else {
        // Hardware card is healthy, but filesystem is unformatted or unsupported (e.g. exFAT)
        // Clean up partial VFS and diskio registration left behind by failed mount
        BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
        if (pdrv != 0xff) {
            char drv[3] = {(char)('0' + pdrv), ':', 0};
            f_mount(0, drv, 0);
            ff_diskio_unregister(pdrv);
        }
        esp_vfs_fat_unregister_path(BSP_SD_MOUNT_POINT);

        s_sd_status = HAL_SD_STATUS_UNFORMATTED;
        s_card_info.is_formatted = false;
        ESP_LOGW(TAG, "SD card (%s, %llu MB) detected, but filesystem is unformatted/unsupported (%s)",
                 s_card_info.name,
                 (unsigned long long)(s_card_info.total_bytes / (1024 * 1024)),
                 esp_err_to_name(mount_ret));
    }

    if (s_sd_status != prev_status) {
        notify_sd_status_changed(s_sd_status);
    }
    return mount_ret;
}

static esp_err_t hal_storage_sd_unmount_internal(void)
{
    if (s_sd_status == HAL_SD_STATUS_MOUNTED && s_card != NULL) {
        esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_host_inited = false;
    } else if (s_sd_status == HAL_SD_STATUS_UNFORMATTED && s_card != NULL) {
        BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
        if (pdrv != 0xff) {
            char drv[3] = {(char)('0' + pdrv), ':', 0};
            f_mount(0, drv, 0);
            ff_diskio_unregister(pdrv);
        }
        esp_vfs_fat_unregister_path(BSP_SD_MOUNT_POINT);
        free(s_card);
        s_card = NULL;
        if (s_host.deinit_p) {
            s_host.deinit_p(s_host.slot);
        }
        s_host_inited = false;
    }
    s_sd_status = HAL_SD_STATUS_NOT_INSERTED;
    memset(&s_card_info, 0, sizeof(s_card_info));
    notify_sd_status_changed(s_sd_status);
    ESP_LOGI(TAG, "SD card unmounted / removed");
    return ESP_OK;
}

esp_err_t hal_storage_sd_mount(void)
{
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    esp_err_t ret = hal_storage_sd_mount_internal();
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ret;
}

esp_err_t hal_storage_sd_unmount(void)
{
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    esp_err_t ret = hal_storage_sd_unmount_internal();
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ret;
}

hal_sd_status_t hal_storage_sd_get_status(void)
{
    hal_sd_status_t status;
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    status = s_sd_status;
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return status;
}

esp_err_t hal_storage_sd_get_info(hal_sd_card_info_t *out_info)
{
    if (!out_info) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    if (s_sd_status == HAL_SD_STATUS_NOT_INSERTED) {
        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    *out_info = s_card_info;
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ESP_OK;
}

static esp_err_t hal_storage_sd_format_internal(void)
{
    ESP_LOGI(TAG, "Formatting SD card at %s to FAT32...", BSP_SD_MOUNT_POINT);

    if (s_sd_status != HAL_SD_STATUS_MOUNTED && s_sd_status != HAL_SD_STATUS_UNFORMATTED) {
        ESP_LOGW(TAG, "Cannot format: no SD card detected in slot");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_card == NULL) {
        ESP_LOGE(TAG, "Card handle is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    // 1. If currently mounted, cleanly unmount and re-probe card handle
    if (s_sd_status == HAL_SD_STATUS_MOUNTED) {
        esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
        s_card = NULL;
        s_host_inited = false;
        if (ensure_host_inited() != ESP_OK) {
            s_sd_status = HAL_SD_STATUS_ERROR;
            notify_sd_status_changed(s_sd_status);
            return ESP_FAIL;
        }
        s_card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
        if (s_card == NULL) {
            return ESP_ERR_NO_MEM;
        }
        esp_err_t probe = sdmmc_card_init(&s_host, s_card);
        if (probe != ESP_OK) {
            ESP_LOGE(TAG, "Failed to re-probe SD card after unmount: %s", esp_err_to_name(probe));
            s_sd_status = HAL_SD_STATUS_ERROR;
            notify_sd_status_changed(s_sd_status);
            return probe;
        }
    } else {
        // Clean any leftover diskio or VFS registration from failed mount probe
        BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
        if (pdrv != 0xff) {
            char drv[3] = {(char)('0' + pdrv), ':', 0};
            f_mount(0, drv, 0);
            ff_diskio_unregister(pdrv);
        }
        esp_vfs_fat_unregister_path(BSP_SD_MOUNT_POINT);
    }

    // 2. Register SDMMC driver to FatFS drive 0
    BYTE pdrv = 0;
    ff_diskio_register_sdmmc(pdrv, s_card);

    // 3. Partition card (MBR with 1 single FAT32 partition taking 100% of card)
    ESP_LOGI(TAG, "Writing fresh partition table (MBR) to SD card...");
    esp_vfs_fat_drive_divide_arr_t partitions = {100, 0, 0, 0};
    esp_err_t part_err = esp_vfs_fat_partition_drive(pdrv, partitions);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "Partitioning failed: %s (0x%x)", esp_err_to_name(part_err), part_err);
        ff_diskio_unregister(pdrv);
        s_sd_status = HAL_SD_STATUS_UNFORMATTED;
        notify_sd_status_changed(s_sd_status);
        return part_err;
    }

    // 4. Format partition as FAT32
    ESP_LOGI(TAG, "Formatting partition to FAT32...");
    const esp_vfs_fat_mount_config_t format_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t fmt_err = esp_vfs_fat_format_drive(pdrv, &format_cfg);
    if (fmt_err != ESP_OK) {
        ESP_LOGE(TAG, "FAT32 format failed: %s (0x%x)", esp_err_to_name(fmt_err), fmt_err);
        ff_diskio_unregister(pdrv);
        s_sd_status = HAL_SD_STATUS_UNFORMATTED;
        notify_sd_status_changed(s_sd_status);
        return fmt_err;
    }

    // 5. Release temporary diskio registration
    ff_diskio_unregister(pdrv);

    // 6. Mount the freshly formatted card
    ESP_LOGI(TAG, "Mounting formatted SD card to %s...", BSP_SD_MOUNT_POINT);
    const esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t mount_ret = esp_vfs_fat_mount_initialized(s_card, BSP_SD_MOUNT_POINT, &mount_cfg);
    if (mount_ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount after format failed: %s (0x%x)", esp_err_to_name(mount_ret), mount_ret);
        BYTE cur_pdrv = ff_diskio_get_pdrv_card(s_card);
        if (cur_pdrv != 0xff) {
            char drv[3] = {(char)('0' + cur_pdrv), ':', 0};
            f_mount(0, drv, 0);
            ff_diskio_unregister(cur_pdrv);
        }
        esp_vfs_fat_unregister_path(BSP_SD_MOUNT_POINT);
        s_sd_status = HAL_SD_STATUS_UNFORMATTED;
        notify_sd_status_changed(s_sd_status);
        return mount_ret;
    }

    s_card_info.is_formatted = true;
    s_sd_status = HAL_SD_STATUS_MOUNTED;
    ESP_LOGI(TAG, "SD card (%s, %llu MB) successfully formatted and mounted at %s",
             s_card_info.name,
             (unsigned long long)(s_card_info.total_bytes / (1024 * 1024)),
             BSP_SD_MOUNT_POINT);
    notify_sd_status_changed(s_sd_status);
    return ESP_OK;
}

esp_err_t hal_storage_sd_format(void)
{
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    esp_err_t ret = hal_storage_sd_format_internal();
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ret;
}

esp_err_t hal_storage_sd_get_usage(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    if (s_sd_status != HAL_SD_STATUS_MOUNTED) {
        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_vfs_fat_info(BSP_SD_MOUNT_POINT, out_total_bytes, out_free_bytes);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_info failed for %s: %s (0x%x)",
                 BSP_SD_MOUNT_POINT, esp_err_to_name(ret), ret);
    } else {
        ESP_LOGD(TAG, "SD usage queried: total=%llu MB, free=%llu MB",
                 out_total_bytes ? (unsigned long long)(*out_total_bytes / (1024 * 1024)) : 0,
                 out_free_bytes ? (unsigned long long)(*out_free_bytes / (1024 * 1024)) : 0);
    }
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    return ret;
}

static void sd_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "Storage auto-detect monitor task started on Core 0");

    // Suppress noisy logs during background probe when card is not inserted
    esp_log_level_set("sdmmc_common", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_req", ESP_LOG_NONE);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
    esp_log_level_set("SD_HOST", ESP_LOG_NONE);

    // Initial boot-time probe/mount
    if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
    ensure_host_inited();
    hal_storage_sd_mount_internal();
    if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);

    // Periodic check loop (every 5 seconds)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (s_sd_mutex) xSemaphoreTake(s_sd_mutex, portMAX_DELAY);
        if (s_sd_status == HAL_SD_STATUS_MOUNTED || s_sd_status == HAL_SD_STATUS_UNFORMATTED) {
            // Health check: verify SD card is still physically present and responding
            if (s_card != NULL) {
                esp_err_t err = sdmmc_get_status(s_card);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "SD card disconnected or removed (%s), unmounting...", esp_err_to_name(err));
                    hal_storage_sd_unmount_internal();
                }
            }
        } else if (s_sd_status == HAL_SD_STATUS_NOT_INSERTED) {
            // Soft-probe: check if card was inserted after boot
            hal_storage_sd_mount_internal();
        }
        if (s_sd_mutex) xSemaphoreGive(s_sd_mutex);
    }
}

esp_err_t hal_storage_sd_init(void)
{
    if (s_monitor_task != NULL) {
        return ESP_OK;
    }

    if (s_sd_mutex == NULL) {
        s_sd_mutex = xSemaphoreCreateMutex();
        if (s_sd_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Pinned to Core 0 with low priority 1, leaving Core 1 entirely dedicated to UI
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
}

/* ---------------------------------------------------------------------------
 * Flash NVS Key-Value Storage API Implementation
 * ------------------------------------------------------------------------- */

esp_err_t hal_nvs_get_blob(const char *part, const char *ns, const char *key, void *out_buf, size_t *inout_len)
{
    if (!ns || !key || !inout_len) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    const char *p = (part && part[0]) ? part : "nvs_app";
    esp_err_t err = nvs_open_from_partition(p, ns, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        err = nvs_open(ns, NVS_READONLY, &handle);
    }
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
    nvs_handle_t handle;
    const char *p = (part && part[0]) ? part : "nvs_app";
    esp_err_t err = nvs_open_from_partition(p, ns, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        err = nvs_open(ns, NVS_READWRITE, &handle);
    }
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
    nvs_handle_t handle;
    const char *p = (part && part[0]) ? part : "nvs_app";
    esp_err_t err = nvs_open_from_partition(p, ns, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        err = nvs_open(ns, NVS_READWRITE, &handle);
    }
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

