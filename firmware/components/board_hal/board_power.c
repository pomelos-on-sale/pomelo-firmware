#include "board_hal_internal.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_power";

// =============================================================================
// AXP2101 PMIC Register Definitions
// =============================================================================
#define AXP2101_I2C_ADDR             0x34
#define AXP2101_SDA_PIN              GPIO_NUM_15
#define AXP2101_SCL_PIN              GPIO_NUM_14

#define AXP2101_REG_STATUS1          0x00  /* Bit 3: Battery present */
#define AXP2101_REG_STATUS2          0x01  /* Bits [7:5]: Charging state */
#define AXP2101_REG_CHIP_ID          0x03  /* Chip identification */
#define AXP2101_REG_PWROFF_CTRL      0x22  /* Power-off settings */
#define AXP2101_REG_POK_SET          0x27  /* Power key ON/OFF timing */
#define AXP2101_REG_ADC_CH_CTRL      0x30  /* ADC channel enable control */
#define AXP2101_REG_VBAT_H           0x34  /* Battery voltage data [12:8] */
#define AXP2101_REG_VBAT_L           0x35  /* Battery voltage data [7:0] */
#define AXP2101_REG_INTEN1           0x41  /* IRQ enable register 1 */
#define AXP2101_REG_INTSTS1          0x48  /* IRQ status register 1 */
#define AXP2101_REG_INTSTS2          0x49  /* IRQ status register 2 */
#define AXP2101_REG_INTSTS3          0x4A  /* IRQ status register 3 */
#define AXP2101_REG_CHG_VOL          0x64  /* Target charging voltage */
#define AXP2101_REG_BAT_DET          0x68  /* Battery detection control */
#define AXP2101_REG_ALDO3_CTRL       0x90  /* ALDO3 power rail (AMOLED) */
#define AXP2101_REG_BAT_PERCENT      0xA4  /* Battery fuel gauge percentage */

// Bitmask constants
#define AXP2101_STATUS1_BATT_PRESENT (1 << 3)
#define AXP2101_CHARGING_MASK        (0x07 << 5)
#define AXP2101_CHARGING_ACTIVE      (0x01 << 5)
#define AXP2101_PEKEY_PRESS_MASK     0x03  /* Bit 1: Short press, Bit 0: Long press */

static i2c_master_dev_handle_t s_axp2101_dev = NULL;
static TaskHandle_t s_power_task = NULL;
static void power_monitor_task(void *arg);

static esp_err_t axp2101_write_reg(uint8_t reg, uint8_t val)
{
    if (!s_axp2101_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_axp2101_dev, buf, sizeof(buf), 1000);
}

static esp_err_t axp2101_read_reg(uint8_t reg, uint8_t *val)
{
    if (!s_axp2101_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_axp2101_dev, &reg, 1, val, 1, 1000);
}

esp_err_t hal_power_init(void)
{
    if (s_axp2101_dev != NULL) {
        return ESP_OK;
    }

    // Attempt to reuse shared I2C master bus handle from BSP
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (i2c_bus == NULL) {
        bsp_i2c_init();
        i2c_bus = bsp_i2c_get_handle();
    }

    if (i2c_bus == NULL) {
        ESP_LOGI(TAG, "Creating dedicated I2C master bus for AXP2101 (SDA:%d, SCL:%d)...",
                 AXP2101_SDA_PIN, AXP2101_SCL_PIN);
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AXP2101_SDA_PIN,
            .scl_io_num = AXP2101_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t ret = i2c_new_master_bus(&bus_cfg, &i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create I2C bus for AXP2101: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_axp2101_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add AXP2101 device to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify communication with AXP2101
    uint8_t chip_id = 0;
    if (axp2101_read_reg(AXP2101_REG_CHIP_ID, &chip_id) == ESP_OK) {
        ESP_LOGI(TAG, "AXP2101 PMIC online (Chip ID: 0x%02X)", chip_id);
    } else {
        ESP_LOGW(TAG, "AXP2101 read chip ID failed, applying register configs anyway...");
    }

    // 1. Disable TS Pin Temperature Measurement to prevent floating 2-pin battery fault
    // Register 0x30: ADC Channel Control (clear bit 1 to disable TS measurement, enable other ADCs)
    uint8_t reg30 = 0;
    if (axp2101_read_reg(AXP2101_REG_ADC_CH_CTRL, &reg30) == ESP_OK) {
        reg30 &= ~(1 << 1); // Disable TS pin measurement
        reg30 |= (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5); // Batt, Vbus, Sys, Temp ADC
        axp2101_write_reg(AXP2101_REG_ADC_CH_CTRL, reg30);
    } else {
        axp2101_write_reg(AXP2101_REG_ADC_CH_CTRL, 0x3D);
    }
    ESP_LOGI(TAG, "AXP2101 TS pin measurement disabled, battery/voltage ADCs enabled");

    // 2. Configure Power-On (128ms) and Power-Off (4s) key timing
    // Register 0x27: bit[1:0]=00 (128ms ON), bit[3:2]=00 (4s OFF), bit[5:4]=01 (1.5s IRQ)
    uint8_t reg27 = 0x10;
    axp2101_write_reg(AXP2101_REG_POK_SET, reg27);
    ESP_LOGI(TAG, "AXP2101 Key Timing set: ON=128ms, OFF=4s, IRQ=1.5s");

    // 3. Configure PWROFF_EN (Register 0x22): Enable long-press power off via PWR key
    // bit 2: 1 (over-temp power off), bit 1: 1 (PWRON > OFFLEVEL power off), bit 0: 0 (power off, not restart)
    axp2101_write_reg(AXP2101_REG_PWROFF_CTRL, 0x06);

    // 4. Enable Battery Detection (Register 0x68, bit 0)
    uint8_t reg68 = 0;
    if (axp2101_read_reg(AXP2101_REG_BAT_DET, &reg68) == ESP_OK) {
        reg68 |= (1 << 0);
        axp2101_write_reg(AXP2101_REG_BAT_DET, reg68);
    }

    // 5. Ensure AMOLED display power rail (ALDO3) is enabled (Register 0x90, bit 2)
    uint8_t reg90 = 0;
    if (axp2101_read_reg(AXP2101_REG_ALDO3_CTRL, &reg90) == ESP_OK) {
        reg90 |= (1 << 2);
        axp2101_write_reg(AXP2101_REG_ALDO3_CTRL, reg90);
    }

    // 6. Set charge target voltage to 4.2V (standard LiPo)
    uint8_t reg64 = 0;
    if (axp2101_read_reg(AXP2101_REG_CHG_VOL, &reg64) == ESP_OK) {
        reg64 = (reg64 & 0xF8) | 0x02; // 4.2V
        axp2101_write_reg(AXP2101_REG_CHG_VOL, reg64);
    }

    // 7. Clear all IRQ status flags & enable PEKEY short press IRQ (Register 0x41 bit 1)
    axp2101_write_reg(AXP2101_REG_INTSTS1, 0xFF);
    axp2101_write_reg(AXP2101_REG_INTSTS2, 0xFF);
    axp2101_write_reg(AXP2101_REG_INTSTS3, 0xFF);
    uint8_t reg41 = 0;
    if (axp2101_read_reg(AXP2101_REG_INTEN1, &reg41) == ESP_OK) {
        reg41 |= (1 << 1) | (1 << 0);
        axp2101_write_reg(AXP2101_REG_INTEN1, reg41);
    }

    if (s_power_task == NULL) {
        xTaskCreatePinnedToCore(power_monitor_task, "board_pwr_mon", 3072, NULL, 1, &s_power_task, 0);
    }

    return ESP_OK;
}

static int32_t s_smoothed_battery_percent = -1;
static int32_t s_percent_window[3] = {-1, -1, -1};
static uint8_t s_window_count = 0;

static int32_t read_raw_battery_percent(void)
{
    if (!s_axp2101_dev) {
        return -1;
    }
    uint8_t status1 = 0;
    if (axp2101_read_reg(AXP2101_REG_STATUS1, &status1) != ESP_OK ||
        !(status1 & AXP2101_STATUS1_BATT_PRESENT)) {
        return -1; // Battery not present
    }
    uint8_t percent = 0;
    if (axp2101_read_reg(AXP2101_REG_BAT_PERCENT, &percent) == ESP_OK) {
        if (percent > 100) percent = 100;
        return (int32_t)percent;
    }
    return -1;
}

static int32_t smooth_battery_percent(int32_t raw_percent)
{
    if (raw_percent < 0) {
        s_window_count = 0;
        s_smoothed_battery_percent = -1;
        return -1;
    }

    if (s_window_count == 0) {
        // Cold boot: fill window with initial reading
        s_percent_window[0] = raw_percent;
        s_percent_window[1] = raw_percent;
        s_percent_window[2] = raw_percent;
        s_window_count = 3;
        s_smoothed_battery_percent = raw_percent;
        return raw_percent;
    }

    // Shift window and push new sample (past 2 + current 1)
    s_percent_window[0] = s_percent_window[1];
    s_percent_window[1] = s_percent_window[2];
    s_percent_window[2] = raw_percent;

    // Integer average with round-half-up: (sum + 1) / 3
    int32_t sum = s_percent_window[0] + s_percent_window[1] + s_percent_window[2];
    int32_t smoothed = (sum + 1) / 3;
    if (smoothed > 100) smoothed = 100;
    if (smoothed < 0) smoothed = 0;

    s_smoothed_battery_percent = smoothed;
    return smoothed;
}

int32_t hal_power_get_battery_percent(void)
{
    if (s_smoothed_battery_percent >= 0) {
        return s_smoothed_battery_percent;
    }
    return read_raw_battery_percent();
}

bool hal_power_is_charging(void)
{
    if (!s_axp2101_dev) {
        return false;
    }
    uint8_t status2 = 0;
    if (axp2101_read_reg(AXP2101_REG_STATUS2, &status2) == ESP_OK) {
        return (status2 & AXP2101_CHARGING_MASK) == AXP2101_CHARGING_ACTIVE;
    }
    return false;
}

int32_t hal_power_get_battery_voltage_mv(void)
{
    if (!s_axp2101_dev) {
        return 0;
    }
    uint8_t h = 0, l = 0;
    if (axp2101_read_reg(AXP2101_REG_VBAT_H, &h) == ESP_OK &&
        axp2101_read_reg(AXP2101_REG_VBAT_L, &l) == ESP_OK) {
        uint16_t raw = (((uint16_t)(h & 0x1F)) << 8) | l;
        return (int32_t)raw;
    }
    return 0;
}

static void power_monitor_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Power monitor task started on Core %d", xPortGetCoreID());

    int32_t last_percent = -1;
    bool last_charging = false;
    uint32_t tick_count = 0;

    // Initial reading
    int32_t raw_init = read_raw_battery_percent();
    last_percent = smooth_battery_percent(raw_init);
    last_charging = hal_power_is_charging();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
        tick_count++;

        if (!s_axp2101_dev) {
            continue;
        }

        // 1. Check AXP2101 PEKEY IRQ status (Power button press)
        uint8_t irq1 = 0;
        if (axp2101_read_reg(AXP2101_REG_INTSTS1, &irq1) == ESP_OK && (irq1 & AXP2101_PEKEY_PRESS_MASK) != 0) {
            axp2101_write_reg(AXP2101_REG_INTSTS1, irq1 & AXP2101_PEKEY_PRESS_MASK);
            hal_event_t ev = {
                .type = HAL_EVENT_BUTTON,
                .data.button = HAL_BUTTON_EVENT_PWR_PRESS,
            };
            hal_event_send(&ev);
        }

        // 2. Check charging state changes (VBUS insert/remove)
        bool charging = hal_power_is_charging();
        bool charging_changed = (charging != last_charging);

        // 3. Check battery percentage (read every 1 second or immediately on charging change)
        bool check_percent = (tick_count % 2 == 0) || charging_changed;
        if (check_percent) {
            int32_t raw_percent = read_raw_battery_percent();
            int32_t percent = smooth_battery_percent(raw_percent);
            int32_t voltage = hal_power_get_battery_voltage_mv();

            if (percent != last_percent || charging_changed) {
                last_percent = percent;
                last_charging = charging;

                hal_power_state_t state = HAL_POWER_STATE_BATTERY_UPDATE;
                if (charging_changed) {
                    state = charging ? HAL_POWER_STATE_CHARGING_STARTED : HAL_POWER_STATE_CHARGING_STOPPED;
                }

                hal_event_t ev = {
                    .type = HAL_EVENT_POWER,
                    .data.power = {
                        .state = state,
                        .percent = percent,
                        .voltage_mv = voltage,
                        .is_charging = charging,
                    },
                };
                hal_event_send(&ev);
            }
        }
    }
}

