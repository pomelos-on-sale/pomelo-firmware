//! MicroSD / storage backend for ESP32-S3.
//!
//! Bridges to the C HAL functions `hal_storage_sd_*`.

use pomelo_hal::{HalError, SdCardInfo, SdCardStatus, StorageBackend, StorageStats};

mod ffi {
    #[repr(C)]
    pub struct HalSdCardInfo {
        pub name: [u8; 16],
        pub total_bytes: u64,
        pub serial_number: u32,
        pub is_formatted: bool,
    }

    extern "C" {
        /// `esp_err_t hal_storage_sd_mount(void)` (0 = `ESP_OK`).
        pub fn hal_storage_sd_mount() -> i32;
        /// `esp_err_t hal_storage_sd_unmount(void)` (0 = `ESP_OK`).
        pub fn hal_storage_sd_unmount() -> i32;
        /// `hal_sd_status_t hal_storage_sd_get_status(void)`.
        pub fn hal_storage_sd_get_status() -> i32;
        /// `esp_err_t hal_storage_sd_get_info(hal_sd_card_info_t *out_info)`.
        pub fn hal_storage_sd_get_info(out_info: *mut HalSdCardInfo) -> i32;
        /// `esp_err_t hal_storage_sd_format(void)`.
        pub fn hal_storage_sd_format() -> i32;
        /// `esp_err_t hal_storage_sd_get_usage(uint64_t *out_total, uint64_t *out_free)`.
        pub fn hal_storage_sd_get_usage(out_total: *mut u64, out_free: *mut u64) -> i32;
    }
}

pub struct EspStorage;

impl EspStorage {
    pub const fn new() -> Self {
        Self
    }
}

impl Default for EspStorage {
    fn default() -> Self {
        Self::new()
    }
}

impl StorageBackend for EspStorage {
    fn init(&mut self) -> Result<(), HalError> {
        // Best-effort non-blocking init; mounting happens on demand or in background task
        Ok(())
    }

    fn sd_status(&self) -> SdCardStatus {
        match unsafe { ffi::hal_storage_sd_get_status() } {
            1 => SdCardStatus::Unformatted,
            2 => SdCardStatus::Mounted,
            3 => SdCardStatus::Error,
            _ => SdCardStatus::NotInserted,
        }
    }

    fn sd_card_info(&self) -> Option<SdCardInfo> {
        let mut info = ffi::HalSdCardInfo {
            name: [0; 16],
            total_bytes: 0,
            serial_number: 0,
            is_formatted: false,
        };
        if unsafe { ffi::hal_storage_sd_get_info(&mut info) } == 0 {
            let name_len = info.name.iter().position(|&b| b == 0).unwrap_or(info.name.len());
            let name = String::from_utf8_lossy(&info.name[..name_len]).trim().to_string();
            Some(SdCardInfo {
                name,
                total_bytes: info.total_bytes,
                serial_number: info.serial_number,
                is_formatted: info.is_formatted,
            })
        } else {
            None
        }
    }

    fn mount_sd(&mut self) -> Result<(), HalError> {
        HalError::from_code(unsafe { ffi::hal_storage_sd_mount() })
    }

    fn unmount_sd(&mut self) -> Result<(), HalError> {
        HalError::from_code(unsafe { ffi::hal_storage_sd_unmount() })
    }

    fn format_sd(&mut self) -> Result<(), HalError> {
        HalError::from_code(unsafe { ffi::hal_storage_sd_format() })
    }

    fn sd_stats(&self) -> Result<StorageStats, HalError> {
        let mut total = 0u64;
        let mut free = 0u64;
        let code = unsafe { ffi::hal_storage_sd_get_usage(&mut total, &mut free) };
        if code == 0 {
            Ok(StorageStats::new(total, free))
        } else {
            Err(HalError::from_code(code).unwrap_err())
        }
    }
}
