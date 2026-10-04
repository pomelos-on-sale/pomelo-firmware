#include "board_hal_internal.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_co5300.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/esp32_s3_touch_amoled_2_16.h"

static const char *TAG = "board_display";

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static SemaphoreHandle_t s_trans_done_sem = NULL;

#if defined(CONFIG_BOARD_LCD_TE_GPIO) && (CONFIG_BOARD_LCD_TE_GPIO >= 0)
#define BSP_LCD_TE CONFIG_BOARD_LCD_TE_GPIO
#define BSP_LCD_TE_ENABLED 1
#else
#define BSP_LCD_TE_ENABLED 0
#endif

static SemaphoreHandle_t s_te_sem = NULL;
static volatile uint32_t s_te_count = 0;
static bool s_te_active = false;
static int64_t s_last_frame_start_us = 0;

#if BSP_LCD_TE_ENABLED
static IRAM_ATTR void s_te_gpio_isr_handler(void *arg)
{
    s_te_count++;
    BaseType_t high_task_awoken = pdFALSE;
    if (s_te_sem) {
        xSemaphoreGiveFromISR(s_te_sem, &high_task_awoken);
    }
    if (high_task_awoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}
#endif

void hal_display_wait_vsync(void)
{
    if (s_te_active && s_te_sem != NULL) {
        // Drain any stale V-sync pulses from previous frames
        xSemaphoreTake(s_te_sem, 0);
        // Wait for next hardware V-Blank rising edge (timeout 25ms in case a pulse is missed)
        if (xSemaphoreTake(s_te_sem, pdMS_TO_TICKS(25)) == pdTRUE) {
            s_last_frame_start_us = esp_timer_get_time();
            return;
        }
        ESP_LOGW(TAG, "Hardware TE timeout (>25ms), fallback to software 60Hz pacing");
        s_te_active = false;
    }

    // Adaptive 60Hz frame pacing fallback (16,667us per frame)
    const int64_t frame_interval_us = 16667;
    int64_t now_us = esp_timer_get_time();

    if (s_last_frame_start_us == 0) {
        s_last_frame_start_us = now_us;
        return;
    }

    int64_t elapsed_us = now_us - s_last_frame_start_us;
    if (elapsed_us < frame_interval_us) {
        int64_t wait_us = frame_interval_us - elapsed_us;
        // Yield FreeRTOS task if we have 2ms or more remaining
        if (wait_us >= 2000) {
            vTaskDelay(pdMS_TO_TICKS((wait_us - 1000) / 1000));
        }
        // Precision microsecond spin wait for exact target boundary
        while ((esp_timer_get_time() - s_last_frame_start_us) < frame_interval_us) {
            esp_rom_delay_us(50);
        }
        s_last_frame_start_us += frame_interval_us;
    } else {
        // Frame took longer than 16.67ms or was idle
        s_last_frame_start_us = esp_timer_get_time();
    }
}

#define CHUNK_LINES_DEFAULT 64
static int s_chunk_lines = CHUNK_LINES_DEFAULT;
static uint16_t *s_dma_chunk[2] = {NULL, NULL};

// =============================================================================
// CO5300 AMOLED Panel Initialization Command Sequence
// =============================================================================
static const co5300_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600}, // Sleep out (requires 120ms+, 600ms safe delay)
    {0xFE, (uint8_t[]){0x20}, 1, 0},   // Select vendor command page 1
    {0x19, (uint8_t[]){0x10}, 1, 0},   // Vendor setting
    {0x1C, (uint8_t[]){0xA0}, 1, 0},   // Vendor setting
    {0xFE, (uint8_t[]){0x00}, 1, 0},   // Select user command page 0
    {0xC4, (uint8_t[]){0x80}, 1, 0},   // SPI mode / interface setting
    {0x3A, (uint8_t[]){0x55}, 1, 0},   // Interface pixel format: 16-bit RGB565
    {0x35, (uint8_t[]){0x00}, 1, 0},   // Tearing effect line output
    {0x53, (uint8_t[]){0x20}, 1, 0},   // Control display (dimming / brightness control enable)
    {0x51, (uint8_t[]){0xFF}, 1, 0},   // Display brightness (0xFF = 100%)
    {0x63, (uint8_t[]){0xFF}, 1, 0},   // HBM brightness setting
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0}, // Column address: 0 to 479 (0x01DF)
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0}, // Row address: 0 to 479 (0x01DF)
    {0x36, (uint8_t[]){0xA0}, 1, 0},   // Memory data access control (scan order / rotation)
    {0x29, (uint8_t[]){0x00}, 0, 600}, // Display ON (with wait)
};

/**
 * @brief High-speed row copy with 32-bit SIMD RGB565 Little-to-Big Endian byte swapping.
 * Processes 2 pixels (4 bytes) per cycle on Xtensa LX7 when word-aligned.
 */
static inline void copy_row_rgb565_be(const uint16_t *src, uint16_t *dst, int32_t width)
{
    if (((uintptr_t)src & 3u) == 0 && ((uintptr_t)dst & 3u) == 0) {
        const uint32_t *src32 = (const uint32_t *)src;
        uint32_t *dst32 = (uint32_t *)dst;
        int32_t words = width / 2;
        for (int32_t i = 0; i < words; ++i) {
            uint32_t val = src32[i];
            dst32[i] = ((val & 0x00FF00FF) << 8) | ((val & 0xFF00FF00) >> 8);
        }
        if (width & 1) {
            dst[width - 1] = __builtin_bswap16(src[width - 1]);
        }
    } else {
        for (int32_t i = 0; i < width; ++i) {
            dst[i] = __builtin_bswap16(src[i]);
        }
    }
}

static IRAM_ATTR bool s_on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                            esp_lcd_panel_io_event_data_t *edata,
                                            void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    if (s_trans_done_sem) {
        xSemaphoreGiveFromISR(s_trans_done_sem, &high_task_awoken);
    }
    return high_task_awoken == pdTRUE;
}

esp_err_t board_display_init(void)
{
    if (s_panel_handle != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Waveshare 480x480 AMOLED display at 40MHz QSPI...");
    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(BSP_LCD_PCLK,
                                                                 BSP_LCD_DATA0,
                                                                 BSP_LCD_DATA1,
                                                                 BSP_LCD_DATA2,
                                                                 BSP_LCD_DATA3,
                                                                 BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t));
    esp_err_t ret = spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_trans_done_sem = xSemaphoreCreateBinary();
    s_te_sem = xSemaphoreCreateBinary();

    // Configure TE GPIO interrupt only if explicitly mapped (e.g. via TP301 jumper wire)
#if BSP_LCD_TE_ENABLED
    gpio_config_t te_gpio_cfg = {
        .pin_bit_mask = BIT64(BSP_LCD_TE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&te_gpio_cfg);
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "gpio_install_isr_service: %s", esp_err_to_name(isr_err));
    }
    gpio_isr_handler_add(BSP_LCD_TE, s_te_gpio_isr_handler, NULL);
#endif

    esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, s_on_color_trans_done, NULL);
    io_config.pclk_hz = 40 * 1000 * 1000; // 40MHz QSPI clock for signal stability
    io_config.trans_queue_depth = 10;

    co5300_vendor_config_t vendor_config = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ret = esp_lcd_new_panel_co5300(s_io_handle, &panel_config, &s_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_co5300 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    // Set 100% brightness (CMD 0x51 with param 0xFF)
    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    uint8_t param = 255;
    esp_lcd_panel_io_tx_param(s_io_handle, lcd_cmd, &param, 1);
    ESP_LOGI(TAG, "AMOLED Display initialized at 40MHz QSPI with 100%% brightness.");

    // Probe hardware TE signal only if mapped
#if BSP_LCD_TE_ENABLED
    vTaskDelay(pdMS_TO_TICKS(50));
    if (s_te_count >= 2) {
        s_te_active = true;
        ESP_LOGI(TAG, "Hardware TE V-Sync interrupt active on GPIO%d (received %lu pulses).", (int)BSP_LCD_TE, (unsigned long)s_te_count);
    } else {
        s_te_active = false;
        ESP_LOGI(TAG, "Hardware TE not detected on GPIO%d. Using adaptive 60Hz frame pacing.", (int)BSP_LCD_TE);
    }
#else
    s_te_active = false;
    ESP_LOGI(TAG, "Hardware TE line not routed to GPIO (test point TP301 on PCB). Using adaptive 60Hz frame pacing.");
#endif
    s_last_frame_start_us = esp_timer_get_time();

    // Allocate 64-line Ping-Pong DMA buffers (60KB each) from internal DMA SRAM
    size_t chunk_bytes = BOARD_DISPLAY_WIDTH * s_chunk_lines * sizeof(uint16_t);
    s_dma_chunk[0] = (uint16_t *)heap_caps_aligned_alloc(64, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_dma_chunk[1] = (uint16_t *)heap_caps_aligned_alloc(64, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!s_dma_chunk[0] || !s_dma_chunk[1]) {
        ESP_LOGW(TAG, "Could not allocate 64-line DMA buffers (120KB), falling back to 32 lines...");
        if (s_dma_chunk[0]) { free(s_dma_chunk[0]); s_dma_chunk[0] = NULL; }
        if (s_dma_chunk[1]) { free(s_dma_chunk[1]); s_dma_chunk[1] = NULL; }
        s_chunk_lines = 32;
        chunk_bytes = BOARD_DISPLAY_WIDTH * s_chunk_lines * sizeof(uint16_t);
        s_dma_chunk[0] = (uint16_t *)heap_caps_aligned_alloc(64, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_dma_chunk[1] = (uint16_t *)heap_caps_aligned_alloc(64, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    ESP_LOGI(TAG, "DMA ping-pong buffers initialized: %d lines (%u bytes each).", s_chunk_lines, (unsigned)chunk_bytes);

    return ESP_OK;
}

void hal_display_draw_bitmap(int32_t x1, int32_t y1, int32_t x2, int32_t y2, const uint16_t *pixels,
                              int32_t stride)
{
    if (!s_panel_handle || !pixels || !s_dma_chunk[0] || !s_dma_chunk[1]) {
        return;
    }
    if (x1 < 0 || y1 < 0 || x2 > BOARD_DISPLAY_WIDTH || y2 > BOARD_DISPLAY_HEIGHT || x1 >= x2 || y1 >= y2) {
        return;
    }
    if (stride < x2) {
        return;
    }

    // Hardware constraint: CO5300 QSPI controller requires 2-pixel aligned coordinates
    x1 = (x1 >> 1) << 1;
    y1 = (y1 >> 1) << 1;
    x2 = ((x2 + 1) >> 1) << 1;
    y2 = ((y2 + 1) >> 1) << 1;
    if (x2 > BOARD_DISPLAY_WIDTH) x2 = BOARD_DISPLAY_WIDTH;
    if (y2 > BOARD_DISPLAY_HEIGHT) y2 = BOARD_DISPLAY_HEIGHT;

    const int32_t width = x2 - x1;
    if (width <= 0) {
        return;
    }

    // Auto-synchronize on the first damage rectangle of a new frame if hal_display_wait_vsync()
    // wasn't already invoked by the presenter:
    int64_t now_us = esp_timer_get_time();
    if (s_last_frame_start_us == 0 || (now_us - s_last_frame_start_us) >= 8000) {
        hal_display_wait_vsync();
    }

    int buf_idx = 0;
    bool dma_pending = false;

    // Addressed from the *snapped* x1, not from the caller's: snapping has to move the source
    // with the rectangle, or a strided caller gets a one-pixel horizontal shift.
    const uint16_t *src = pixels + (size_t)y1 * (size_t)stride + (size_t)x1;

    for (int32_t y = y1; y < y2; y += s_chunk_lines) {
        int32_t chunk_h = (y + s_chunk_lines <= y2) ? s_chunk_lines : (y2 - y);

        // Byte-swapped into the ping-pong chunk here because the panel wants big-endian RGB565,
        // and contiguous because `esp_lcd_panel_draw_bitmap` takes a single buffer -- so a row
        // at a time whenever the source is strided.
        for (int32_t r = 0; r < chunk_h; ++r) {
            const uint16_t *in = src + (size_t)r * (size_t)stride;
            uint16_t *out = s_dma_chunk[buf_idx] + (size_t)r * (size_t)width;
            copy_row_rgb565_be(in, out, width);
        }

        // Wait for previous DMA transaction to complete before queuing new one
        if (dma_pending) {
            xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
            dma_pending = false;
        }

        // Send over QSPI via DMA
        esp_lcd_panel_draw_bitmap(s_panel_handle, x1, y, x2, y + chunk_h, s_dma_chunk[buf_idx]);
        dma_pending = true;

        buf_idx = 1 - buf_idx;
        src += (size_t)chunk_h * (size_t)stride;
    }

    // Wait for the final chunk's DMA transfer to finish
    if (dma_pending) {
        xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
    }
}

void hal_display_set_power(bool on)
{
    if (s_panel_handle) {
        ESP_LOGI(TAG, "Setting AMOLED display power: %s", on ? "ON" : "OFF");
        esp_lcd_panel_disp_on_off(s_panel_handle, on);
    }
}

