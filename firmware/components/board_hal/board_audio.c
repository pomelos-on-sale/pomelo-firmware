#include "board_hal_internal.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_pthread.h"
#include "esp_heap_caps.h"

static const char *TAG = "board_audio";

#if BOARD_HAS_AUDIO_SPEAKER

#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"

static esp_codec_dev_handle_t s_spk_codec_dev = NULL;
static bool s_audio_opened = false;
static SemaphoreHandle_t s_audio_mutex = NULL;
static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static i2s_chan_handle_t s_i2s_rx_chan = NULL;
static const audio_codec_data_if_t *s_i2s_data_if = NULL;

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

static esp_err_t audio_i2s_init(void)
{
    if (s_i2s_data_if != NULL) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_PIN_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BOARD_PIN_I2S_MCLK,
            .bclk = BOARD_PIN_I2S_BCLK,
            .ws = BOARD_PIN_I2S_WS,
            .dout = BOARD_PIN_I2S_DOUT,
            .din = BOARD_PIN_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    if (s_i2s_tx_chan) {
        ret = i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg);
        if (ret != ESP_OK) goto err;
        ret = i2s_channel_enable(s_i2s_tx_chan);
        if (ret != ESP_OK) goto err;
    }
    if (s_i2s_rx_chan) {
        ret = i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg);
        if (ret != ESP_OK) goto err;
        ret = i2s_channel_enable(s_i2s_rx_chan);
        if (ret != ESP_OK) goto err;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BOARD_PIN_I2S_NUM,
        .rx_handle = s_i2s_rx_chan,
        .tx_handle = s_i2s_tx_chan,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!s_i2s_data_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        ret = ESP_FAIL;
        goto err;
    }

    return ESP_OK;

err:
    if (s_i2s_tx_chan) {
        i2s_del_channel(s_i2s_tx_chan);
        s_i2s_tx_chan = NULL;
    }
    if (s_i2s_rx_chan) {
        i2s_del_channel(s_i2s_rx_chan);
        s_i2s_rx_chan = NULL;
    }
    return ret;
}

static esp_codec_dev_handle_t audio_codec_speaker_init(void)
{
    if (audio_i2s_init() != ESP_OK) {
        return NULL;
    }

    i2c_master_bus_handle_t i2c_bus = hal_i2c_get_master_bus();
    if (!i2c_bus) {
        ESP_LOGE(TAG, "No I2C bus available for ES8311 codec");
        return NULL;
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BOARD_PIN_I2C_NUM,
        .addr = BOARD_PIN_AUDIO_ES8311_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!i2c_ctrl_if) {
        ESP_LOGE(TAG, "Failed to create I2C control interface for ES8311");
        return NULL;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BOARD_PIN_AUDIO_PA,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (!es8311_dev) {
        ESP_LOGE(TAG, "Failed to create ES8311 codec device");
        return NULL;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = s_i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
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
    ESP_LOGI(TAG, "Initializing ES8311 audio speaker codec via native ESP-IDF driver...");
    s_spk_codec_dev = audio_codec_speaker_init();
    if (!s_spk_codec_dev) {
        ESP_LOGE(TAG, "audio_codec_speaker_init failed");
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
    // Defensive safety check: ensure adequate internal DMA memory before attempting hardware open/reconfig.
    size_t free_dma = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t largest_dma_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (free_dma < 1024 || largest_dma_block < 512) {
        ESP_LOGW(TAG, "Insufficient internal DMA memory to open audio: free=%u, largest_block=%u",
                 (unsigned)free_dma, (unsigned)largest_dma_block);
        audio_unlock();
        return ESP_ERR_NO_MEM;
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
    uint8_t silence[1024] = {0};
    esp_codec_dev_write(s_spk_codec_dev, silence, sizeof(silence));
    audio_unlock();

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

#else // !BOARD_HAS_AUDIO_SPEAKER

static uint8_t s_null_audio_vol = 75;

esp_err_t hal_audio_init(void)
{
    ESP_LOGI(TAG, "Audio hardware bypassed: Null Audio Sink activated");
    return ESP_OK;
}

esp_err_t hal_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample)
{
    ESP_LOGI(TAG, "Null Audio stream opened: %u Hz, %u ch, %u-bit",
             (unsigned)sample_rate, channels, bits_per_sample);
    return ESP_OK;
}

esp_err_t hal_audio_write(const void *data, uint32_t len)
{
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t hal_audio_drain(void)
{
    return ESP_OK;
}

esp_err_t hal_audio_set_volume(uint8_t volume)
{
    s_null_audio_vol = volume > 100 ? 100 : volume;
    return ESP_OK;
}

esp_err_t hal_audio_close(void)
{
    return ESP_OK;
}

#endif // BOARD_HAS_AUDIO_SPEAKER

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
