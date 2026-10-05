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

/*
#if __has_include("splash_logo.h")
#include "splash_logo.h"
#define HAS_SPLASH_LOGO 1
#else
#define HAS_SPLASH_LOGO 0
#endif
*/

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
static bool s_display_active = false;

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

#define CHUNK_STRIP_WIDTH_DEFAULT 64
static int s_chunk_strip_width = CHUNK_STRIP_WIDTH_DEFAULT;
static uint16_t *s_dma_chunk[2] = {NULL, NULL};

// =============================================================================
// CO5300 AMOLED Panel Initialization Command Sequence
// =============================================================================
static const co5300_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120}, // Sleep out (requires 120ms standard delay per CO5300 datasheet)
    {0xFE, (uint8_t[]){0x20}, 1, 0},   // Select vendor command page 1
    {0x19, (uint8_t[]){0x10}, 1, 0},   // Vendor setting
    {0x1C, (uint8_t[]){0xA0}, 1, 0},   // Vendor setting
    {0xFE, (uint8_t[]){0x00}, 1, 0},   // Select user command page 0
    {0xC4, (uint8_t[]){0x80}, 1, 0},   // SPI mode / interface setting
    {0x3A, (uint8_t[]){0x55}, 1, 0},   // Interface pixel format: 16-bit RGB565
    {0x35, (uint8_t[]){0x00}, 1, 0},   // Tearing effect line output
    {0x53, (uint8_t[]){0x20}, 1, 0},   // Control display (dimming / brightness control enable)
    {0x51, (uint8_t[]){0x00}, 1, 0},   // Display brightness (0% initially to prevent power-on flash)
    {0x63, (uint8_t[]){0xFF}, 1, 0},   // HBM brightness setting
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0}, // Column address: 0 to 479 (0x01DF)
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0}, // Row address: 0 to 479 (0x01DF)
    {0x36, (uint8_t[]){0xA0}, 1, 0},   // Memory data access control (scan order / rotation)
    {0x28, (uint8_t[]){0x00}, 0, 0},   // Display OFF until first frame is presented
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

    ESP_LOGI(TAG, "Initializing Waveshare 480x480 AMOLED display at 60MHz QSPI (high drive strength)...");
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

    // Boost QSPI GPIO output drive capability to maximum (40mA, Cap 3) for clean 60MHz signal edges
    gpio_set_drive_capability(BSP_LCD_PCLK, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(BSP_LCD_DATA0, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(BSP_LCD_DATA1, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(BSP_LCD_DATA2, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(BSP_LCD_DATA3, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(BSP_LCD_CS, GPIO_DRIVE_CAP_3);

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
    io_config.pclk_hz = 60 * 1000 * 1000; // 60MHz high-speed QSPI clock
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
    // Keep display blanked/off until first valid frame is written to prevent power-on green flash
    esp_lcd_panel_disp_on_off(s_panel_handle, false);
    ESP_LOGI(TAG, "AMOLED panel controller initialized (standby mode, blanked).");

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

    // Allocate vertical strip Ping-Pong DMA buffers (480 * 64 * 2 = 60KB each) from internal DMA SRAM
    size_t chunk_bytes = BOARD_DISPLAY_HEIGHT * s_chunk_strip_width * sizeof(uint16_t);
    s_dma_chunk[0] = (uint16_t *)heap_caps_aligned_alloc(64, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_dma_chunk[1] = (uint16_t *)heap_caps_aligned_alloc(64, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!s_dma_chunk[0] || !s_dma_chunk[1]) {
        ESP_LOGW(TAG, "Could not allocate 64-col DMA buffers (120KB), falling back to 32 cols...");
        if (s_dma_chunk[0]) { free(s_dma_chunk[0]); s_dma_chunk[0] = NULL; }
        if (s_dma_chunk[1]) { free(s_dma_chunk[1]); s_dma_chunk[1] = NULL; }
        s_chunk_strip_width = 32;
        chunk_bytes = BOARD_DISPLAY_HEIGHT * s_chunk_strip_width * sizeof(uint16_t);
        s_dma_chunk[0] = (uint16_t *)heap_caps_aligned_alloc(64, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_dma_chunk[1] = (uint16_t *)heap_caps_aligned_alloc(64, chunk_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    ESP_LOGI(TAG, "DMA ping-pong buffers initialized: %d column strips (%u bytes each).", s_chunk_strip_width, (unsigned)chunk_bytes);

    // Zero-clear entire GRAM to pure black (0x0000) so uninitialized memory is never visible
    memset(s_dma_chunk[0], 0, chunk_bytes);
    for (int32_t x = 0; x < BOARD_DISPLAY_WIDTH; x += s_chunk_strip_width) {
        esp_lcd_panel_draw_bitmap(s_panel_handle, x, 0, x + s_chunk_strip_width, BOARD_DISPLAY_HEIGHT, s_dma_chunk[0]);
        xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
    }

/*
#if HAS_SPLASH_LOGO
    // Draw centered "Pomelo UI" splash logo
    int32_t splash_x1 = (BOARD_DISPLAY_WIDTH - SPLASH_LOGO_WIDTH) / 2;
    int32_t splash_y1 = (BOARD_DISPLAY_HEIGHT - SPLASH_LOGO_HEIGHT) / 2;
    int32_t splash_x2 = splash_x1 + SPLASH_LOGO_WIDTH;
    int32_t splash_y2 = splash_y1 + SPLASH_LOGO_HEIGHT;

    // CO5300 QSPI requires 2-pixel alignment
    splash_x1 = (splash_x1 >> 1) << 1;
    splash_y1 = (splash_y1 >> 1) << 1;
    splash_x2 = ((splash_x2 + 1) >> 1) << 1;
    splash_y2 = ((splash_y2 + 1) >> 1) << 1;

    size_t splash_bytes = sizeof(s_splash_logo);
    if (splash_bytes <= chunk_bytes) {
        memcpy(s_dma_chunk[0], s_splash_logo, splash_bytes);
        esp_lcd_panel_draw_bitmap(s_panel_handle, splash_x1, splash_y1, splash_x2, splash_y2, s_dma_chunk[0]);
        xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
    }

    // Turn on display and full brightness seamlessly now that splash logo is in GRAM
    esp_lcd_panel_disp_on_off(s_panel_handle, true);
    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    uint8_t param = 255;
    esp_lcd_panel_io_tx_param(s_io_handle, lcd_cmd, &param, 1);
    s_display_active = true;
    ESP_LOGI(TAG, "AMOLED display output enabled with Pomelo UI splash screen.");
#endif
*/

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
    const int32_t height = y2 - y1;
    if (width <= 0 || height <= 0) {
        return;
    }

    // Only auto-synchronize if no frame pacing timestamp was recorded at all
    if (s_last_frame_start_us == 0) {
        hal_display_wait_vsync();
    }

    int buf_idx = 0;
    bool dma_pending = false;

    // Scan direction matching physical Gate driver scan: Right to Left (x2 -> x1).
    // Slicing vertically along X makes DMA progression parallel to the physical OLED scan,
    // breaking the 45-degree diagonal tearing plane into a parallel beam chasing model.
    for (int32_t x = x2; x > x1; ) {
        int32_t cur_x1 = (x - s_chunk_strip_width >= x1) ? (x - s_chunk_strip_width) : x1;
        int32_t cur_x2 = x;
        int32_t strip_w = cur_x2 - cur_x1;

        // Byte-swap into the ping-pong chunk buffer
        for (int32_t r = 0; r < height; ++r) {
            const uint16_t *in = pixels + (size_t)(y1 + r) * (size_t)stride + (size_t)cur_x1;
            uint16_t *out = s_dma_chunk[buf_idx] + (size_t)r * (size_t)strip_w;
            copy_row_rgb565_be(in, out, strip_w);
        }

        // Wait for previous DMA transaction to complete before queuing new one
        if (dma_pending) {
            xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
            dma_pending = false;
        }

        // Send over QSPI via DMA
        esp_lcd_panel_draw_bitmap(s_panel_handle, cur_x1, y1, cur_x2, y2, s_dma_chunk[buf_idx]);
        dma_pending = true;

        buf_idx = 1 - buf_idx;
        x = cur_x1;
    }

    // Wait for the final chunk's DMA transfer to finish
    if (dma_pending) {
        xSemaphoreTake(s_trans_done_sem, portMAX_DELAY);
    }

    // Turn on display and full brightness seamlessly once the first valid frame is written
    if (!s_display_active) {
        esp_lcd_panel_disp_on_off(s_panel_handle, true);
        uint32_t lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= 0x02 << 24;
        uint8_t param = 255;
        esp_lcd_panel_io_tx_param(s_io_handle, lcd_cmd, &param, 1);
        s_display_active = true;
        ESP_LOGI(TAG, "AMOLED display output enabled on first frame.");
    }
}

void hal_display_set_power(bool on)
{
    if (s_panel_handle) {
        ESP_LOGI(TAG, "Setting AMOLED display power: %s", on ? "ON" : "OFF");
        esp_lcd_panel_disp_on_off(s_panel_handle, on);
        s_display_active = on;
    }
}

