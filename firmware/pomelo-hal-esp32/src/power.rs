//! AXP2101 power backend (the board's PMIC).
//!
//! Raw `extern "C"` bindings for the `hal_power_*` C API live in the private
//! `ffi` module below; the safe trait impl is a thin wrapper over them.

use pomelo_hal::{HalError, PowerBackend};

mod ffi {
    extern "C" {
        /// `esp_err_t hal_power_init(void)` (0 = `ESP_OK`).
        pub fn hal_power_init() -> i32;
        /// `int32_t hal_power_get_battery_percent(void)`; negative = unavailable.
        pub fn hal_power_get_battery_percent() -> i32;
        /// `bool hal_power_is_charging(void)`.
        pub fn hal_power_is_charging() -> bool;
        /// `int32_t hal_power_get_battery_voltage_mv(void)`; 0 = unavailable.
        pub fn hal_power_get_battery_voltage_mv() -> i32;
    }
}

pub struct EspPower;

impl EspPower {
    pub const fn new() -> Self {
        Self
    }
}

impl Default for EspPower {
    fn default() -> Self {
        Self::new()
    }
}

impl PowerBackend for EspPower {
    fn init(&mut self) -> Result<(), HalError> {
        // Idempotent on the C side; returns ESP_OK if the PMIC is already set up.
        HalError::from_code(unsafe { ffi::hal_power_init() })
    }

    #[inline]
    fn battery_percent(&self) -> Result<u8, HalError> {
        let raw = unsafe { ffi::hal_power_get_battery_percent() };
        if raw < 0 {
            Err(HalError::NotSupported) // battery absent / unknown
        } else {
            Ok(raw.min(100) as u8)
        }
    }

    #[inline]
    fn is_charging(&self) -> Result<bool, HalError> {
        Ok(unsafe { ffi::hal_power_is_charging() })
    }

    #[inline]
    fn battery_voltage_mv(&self) -> Result<u32, HalError> {
        let raw = unsafe { ffi::hal_power_get_battery_voltage_mv() };
        if raw <= 0 {
            Err(HalError::NotSupported)
        } else {
            Ok(raw as u32)
        }
    }
}
