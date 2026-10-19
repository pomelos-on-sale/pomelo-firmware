#include "board_hal_internal.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_pthread.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "board_audio";

static esp_codec_dev_handle_t s_spk_codec_dev = NULL;
static bool s_audio_opened = false;
static SemaphoreHandle_t s_audio_mutex = NULL;

static void audio_lock(void)
{
    if (!s_audio_mutex) {
        s_audio_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (s_audio_mutex) {
        xSemaphoreTakeRecursive(s_audio_mutex, portMAX_DELAY);
    }
}

static void audio_unlock(void)
{
    if (s_audio_mutex) {
        xSemaphoreGiveRecursive(s_audio_mutex);
    }
}

/* ---------------------------------------------------------------------------
 * Primary Audio Hardware Control API (Audio Sink)
 * ------------------------------------------------------------------------- */

esp_err_t hal_audio_init(void)
{
    audio_lock();
    if (s_spk_codec_dev != NULL) {
        audio_unlock();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Initializing ES8311 audio speaker codec via Waveshare BSP...");
    s_spk_codec_dev = bsp_audio_codec_speaker_init();
    if (!s_spk_codec_dev) {
        ESP_LOGE(TAG, "bsp_audio_codec_speaker_init failed");
        audio_unlock();
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk_codec_dev, 75);
    ESP_LOGI(TAG, "ES8311 audio speaker codec initialized successfully.");
    audio_unlock();
    return ESP_OK;
}

esp_err_t hal_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample)
{
    audio_lock();
    if (!s_spk_codec_dev) {
        if (hal_audio_init() != ESP_OK) {
            audio_unlock();
            return ESP_FAIL;
        }
    }
    if (s_audio_opened) {
        esp_codec_dev_close(s_spk_codec_dev);
        s_audio_opened = false;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits_per_sample,
        .channel = channels,
        .channel_mask = 0,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    int ret = esp_codec_dev_open(s_spk_codec_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d (rate=%u, ch=%u, bits=%u)",
                 ret, (unsigned)sample_rate, channels, bits_per_sample);
        audio_unlock();
        return ESP_FAIL;
    }
    s_audio_opened = true;
    ESP_LOGI(TAG, "Audio stream opened: %u Hz, %u ch, %u-bit",
             (unsigned)sample_rate, channels, bits_per_sample);
    audio_unlock();
    return ESP_OK;
}

esp_err_t hal_audio_write(const void *data, uint32_t len)
{
    audio_lock();
    if (!s_spk_codec_dev || !s_audio_opened || !data || len == 0) {
        audio_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    int ret = esp_codec_dev_write(s_spk_codec_dev, (void *)data, (int)len);
    audio_unlock();
    if (ret != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t hal_audio_drain(void)
{
    audio_lock();
    if (!s_spk_codec_dev || !s_audio_opened) {
        audio_unlock();
        return ESP_OK;
    }
    // Feed a small silence buffer to push hardware FIFO through I2S DMA
    uint8_t silence[1024] = {0};
    esp_codec_dev_write(s_spk_codec_dev, silence, sizeof(silence));
    audio_unlock();

    // Delay briefly to allow DMA pipeline to flush without tail clipping
    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t hal_audio_set_volume(uint8_t volume)
{
    audio_lock();
    if (!s_spk_codec_dev) {
        audio_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    int vol = volume > 100 ? 100 : volume;
    int ret = esp_codec_dev_set_out_vol(s_spk_codec_dev, vol);
    audio_unlock();
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t hal_audio_close(void)
{
    audio_lock();
    if (s_spk_codec_dev && s_audio_opened) {
        esp_codec_dev_close(s_spk_codec_dev);
        s_audio_opened = false;
        ESP_LOGI(TAG, "Audio stream closed.");
    }
    audio_unlock();
    return ESP_OK;
}

esp_err_t hal_audio_configure_pthread(uint32_t stack_size, bool use_psram)
{
    if (stack_size == 0) {
        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        return esp_pthread_set_cfg(&cfg);
    }
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = stack_size;
    cfg.thread_name = "audio_worker";
    if (use_psram) {
        cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    } else {
        cfg.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
    return esp_pthread_set_cfg(&cfg);
}
