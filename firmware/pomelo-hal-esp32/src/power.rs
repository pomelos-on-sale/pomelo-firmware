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
        /// `void hal_power_reboot(void)`.
        pub fn hal_power_reboot();
        /// `bool hal_power_is_usb_connected(void)`.
        pub fn hal_power_is_usb_connected() -> bool;
        /// `void hal_power_set_stay_awake_on_usb(bool enable)`.
        pub fn hal_power_set_stay_awake_on_usb(enable: bool);
        /// `bool hal_power_get_stay_awake_on_usb(void)`.
        pub fn hal_power_get_stay_awake_on_usb() -> bool;
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

    #[inline]
    fn set_display_power(&mut self, on: bool) -> Result<(), HalError> {
        extern "C" {
            fn hal_display_set_power(on: bool);
        }
        unsafe { hal_display_set_power(on) };
        Ok(())
    }

    #[inline]
    fn is_display_on(&self) -> Result<bool, HalError> {
        extern "C" {
            fn hal_display_is_active() -> bool;
        }
        Ok(unsafe { hal_display_is_active() })
    }

    #[inline]
    fn set_screen_off_timeout(&mut self, timeout: Option<std::time::Duration>) -> Result<(), HalError> {
        extern "C" {
            fn hal_display_set_screen_off_timeout_ms(timeout_ms: u32);
        }
        let ms = timeout
            .map(|d| d.as_millis().min(u32::MAX as u128) as u32)
            .unwrap_or(0);
        unsafe { hal_display_set_screen_off_timeout_ms(ms) };
        Ok(())
    }

    #[inline]
    fn screen_off_timeout(&self) -> Result<Option<std::time::Duration>, HalError> {
        extern "C" {
            fn hal_display_get_screen_off_timeout_ms() -> u32;
        }
        let ms = unsafe { hal_display_get_screen_off_timeout_ms() };
        if ms == 0 {
            Ok(None)
        } else {
            Ok(Some(std::time::Duration::from_millis(ms as u64)))
        }
    }

    #[inline]
    fn reboot(&mut self) -> Result<(), HalError> {
        unsafe { ffi::hal_power_reboot() };
        Ok(())
    }

    #[inline]
    fn is_usb_connected(&self) -> Result<bool, HalError> {
        Ok(unsafe { ffi::hal_power_is_usb_connected() })
    }

    #[inline]
    fn stay_awake_on_usb(&self) -> Result<bool, HalError> {
        Ok(unsafe { ffi::hal_power_get_stay_awake_on_usb() })
    }

    #[inline]
    fn set_stay_awake_on_usb(&mut self, enable: bool) -> Result<(), HalError> {
        unsafe { ffi::hal_power_set_stay_awake_on_usb(enable) };
        Ok(())
    }
}
