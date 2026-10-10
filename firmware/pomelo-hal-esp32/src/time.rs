use std::ffi::CString;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use pomelo_hal::{HalError, TimeBackend};

mod ffi {
    use std::os::raw::{c_char, c_int};

    extern "C" {
        pub fn hal_sntp_sync(server: *const c_char, timeout_ms: u32) -> c_int;
        pub fn hal_sntp_is_synced() -> bool;
    }
}

/// ESP32-S3 hardware SNTP time backend.
pub struct EspTime;

impl EspTime {
    pub fn new() -> Self {
        Self
    }
}

impl Default for EspTime {
    fn default() -> Self {
        Self::new()
    }
}

impl TimeBackend for EspTime {
    fn sync(&mut self, server: Option<&str>, timeout: Duration) -> Result<u64, HalError> {
        let server_c = server.and_then(|s| CString::new(s).ok());
        let server_ptr = server_c.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
        let timeout_ms = timeout.as_millis().min(u32::MAX as u128) as u32;

        let ret = unsafe { ffi::hal_sntp_sync(server_ptr, timeout_ms) };
        if ret == 0 {
            Ok(self.now_unix())
        } else {
            Err(HalError::Internal(ret))
        }
    }

    fn is_synced(&self) -> bool {
        unsafe { ffi::hal_sntp_is_synced() }
    }

    fn now_unix(&self) -> u64 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0)
    }
}
