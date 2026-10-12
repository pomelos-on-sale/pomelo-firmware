use std::ffi::CString;

use pomelo_hal::{HalError, HttpBackend, HttpState};

mod ffi {
    use std::os::raw::{c_char, c_int};

    #[allow(dead_code)]
    #[repr(C)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum HalHttpState {
        Idle = 0,
        Running = 1,
        Done = 2,
        Error = 3,
    }

    #[repr(C)]
    #[derive(Debug, Clone, Copy)]
    pub struct HalHttpStatusSnapshot {
        pub state: HalHttpState,
        pub downloaded_bytes: usize,
        pub total_bytes: usize,
        pub status_code: c_int,
        pub error_code: c_int,
    }

    extern "C" {
        pub fn hal_http_init() -> c_int;
        pub fn hal_http_start(url: *const c_char) -> c_int;
        pub fn hal_http_get_status() -> HalHttpStatusSnapshot;
        pub fn hal_http_get_result_size() -> usize;
        pub fn hal_http_take_result(out_buf: *mut u8, max_len: usize) -> c_int;
        pub fn hal_http_cancel();
        pub fn hal_sntp_is_synced() -> bool;
    }
}

/// ESP32-S3 hardware HTTP / HTTPS client backend.
pub struct EspHttp;

impl EspHttp {
    pub fn new() -> Self {
        unsafe { ffi::hal_http_init() };
        Self
    }
}

impl Default for EspHttp {
    fn default() -> Self {
        Self::new()
    }
}

impl HttpBackend for EspHttp {
    fn start(&mut self, url: &str) -> Result<(), HalError> {
        // Guard against TLS certificate validation failure before time is synchronized
        if url.starts_with("https://") && !unsafe { ffi::hal_sntp_is_synced() } {
            return Err(HalError::ClockNotSynced);
        }

        let url_c = CString::new(url).map_err(|_| HalError::InvalidArg)?;
        let ret = unsafe { ffi::hal_http_start(url_c.as_ptr()) };
        if ret == 0 {
            Ok(())
        } else {
            Err(HalError::Internal(ret))
        }
    }

    fn state(&self) -> HttpState {
        let snap = unsafe { ffi::hal_http_get_status() };
        match snap.state {
            ffi::HalHttpState::Idle => HttpState::Idle,
            ffi::HalHttpState::Running => HttpState::Running {
                downloaded: snap.downloaded_bytes,
                total: if snap.total_bytes > 0 {
                    Some(snap.total_bytes)
                } else {
                    None
                },
            },
            ffi::HalHttpState::Done => HttpState::Done,
            ffi::HalHttpState::Error => HttpState::Error,
        }
    }

    fn status_code(&self) -> Option<u16> {
        let snap = unsafe { ffi::hal_http_get_status() };
        if snap.status_code > 0 {
            Some(snap.status_code as u16)
        } else {
            None
        }
    }

    fn take_result(&mut self) -> Result<Vec<u8>, HalError> {
        let snap = unsafe { ffi::hal_http_get_status() };
        if snap.state != ffi::HalHttpState::Done {
            return Err(HalError::InvalidState);
        }

        let size = unsafe { ffi::hal_http_get_result_size() };
        if size == 0 {
            let mut dummy = [0u8; 1];
            let _ = unsafe { ffi::hal_http_take_result(dummy.as_mut_ptr(), 0) };
            return Ok(Vec::new());
        }

        let mut buf = vec![0u8; size];
        let read = unsafe { ffi::hal_http_take_result(buf.as_mut_ptr(), size) };
        if read < 0 {
            Err(HalError::Internal(read))
        } else {
            buf.truncate(read as usize);
            Ok(buf)
        }
    }

    fn cancel(&mut self) -> Result<(), HalError> {
        unsafe { ffi::hal_http_cancel() };
        Ok(())
    }
}
