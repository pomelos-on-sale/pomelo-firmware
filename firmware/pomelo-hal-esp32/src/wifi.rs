//! Wi-Fi backend (ESP-IDF `esp_wifi`, via `firmware/components/board_hal/board_wifi.c`).
//!
//! Raw `extern "C"` bindings live in the private `ffi` module below; the safe
//! trait impl is a thin wrapper that maps the C status codes and `char[]`
//! buffers into Rust types. All calls are non-blocking — the C side caches
//! state in its event handlers.

use std::ffi::{c_char, CString};
use std::sync::Mutex;

use pomelo_hal::wifi_credentials::{self, WifiCredentials};
use pomelo_hal::{ApInfo, HalError, ScanState, WifiBackend, WifiState, WifiStatus};

mod ffi {
    use std::ffi::c_char;

    pub const SSID_MAX_LEN: usize = 33;
    pub const IP_MAX_LEN: usize = 16;
    pub const MAX_AP_RECORDS: usize = 20;

    /// Mirrors `hal_wifi_ap_t`.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct HalWifiAp {
        pub ssid: [c_char; SSID_MAX_LEN],
        pub rssi: i8,
        pub channel: u8,
        pub secure: bool,
    }

    /// Mirrors `hal_wifi_status_t`.
    #[repr(C)]
    #[derive(Clone, Copy)]
    pub struct HalWifiStatus {
        pub state: i32,
        pub ssid: [c_char; SSID_MAX_LEN],
        pub ip: [c_char; IP_MAX_LEN],
        pub netmask: [c_char; IP_MAX_LEN],
        pub gateway: [c_char; IP_MAX_LEN],
        pub rssi: i8,
    }

    extern "C" {
        pub fn hal_wifi_init() -> i32;
        pub fn hal_wifi_set_enabled(on: bool) -> i32;
        pub fn hal_wifi_is_enabled() -> bool;
        pub fn hal_wifi_scan_start() -> i32;
        pub fn hal_wifi_scan_get_state() -> i32;
        pub fn hal_wifi_scan_get_results(
            records: *mut HalWifiAp,
            max_count: u16,
            out_count: *mut u16,
        ) -> i32;
        pub fn hal_wifi_connect(ssid: *const c_char, password: *const c_char) -> i32;
        pub fn hal_wifi_disconnect() -> i32;
        pub fn hal_wifi_get_status(out_status: *mut HalWifiStatus) -> i32;
    }
}

// C state codes (see board_hal.h).
const SCAN_STATE_SCANNING: i32 = 1;
const SCAN_STATE_DONE: i32 = 2;
const SCAN_STATE_ERROR: i32 = -1;

const STATE_SCANNING: i32 = 1;
const STATE_CONNECTING: i32 = 2;
const STATE_CONNECTED: i32 = 3;

fn c_buf_to_string(buf: &[c_char]) -> String {
    let len = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    let bytes = unsafe { std::slice::from_raw_parts(buf.as_ptr() as *const u8, len) };
    String::from_utf8_lossy(bytes).into_owned()
}

pub struct EspWifi;

impl EspWifi {
    pub const fn new() -> Self {
        Self
    }
}

impl Default for EspWifi {
    fn default() -> Self {
        Self::new()
    }
}

/// The network the file named when this backend was initialised.
///
/// A `static` and not a field, because `EspWifi` is a unit struct the board builds in a `const`
/// context, and a cache is not worth giving that up. The file is read once, in `init`, and never
/// again: `saved` is called by whatever draws the page, and a filesystem read behind a getter is a
/// cost nobody would see coming.
static SAVED: Mutex<Option<WifiCredentials>> = Mutex::new(None);

/// A failed filesystem call, as the HAL's error type.
fn io_error(error: std::io::Error) -> HalError {
    HalError::Io(error.to_string())
}

impl WifiBackend for EspWifi {
    fn init(&mut self) -> Result<(), HalError> {
        // Read once, here. `Ok(None)` is a board that has never been on a network and is not worth a
        // line; an `Err` is a file that is there and unreadable, which is a different problem from
        // no file at all and must not be reported as one.
        match WifiCredentials::load(wifi_credentials::BOARD_APP_DATA) {
            Ok(saved) => {
                if let Some(saved) = &saved {
                    eprintln!(
                        "[wifi] remembered {:?} (autoconnect={})",
                        saved.ssid, saved.autoconnect
                    );
                }
                *SAVED.lock().unwrap() = saved;
            }
            Err(error) => eprintln!("[wifi] the credentials file is unreadable: {error}"),
        }

        HalError::from_code(unsafe { ffi::hal_wifi_init() })?;

        Ok(())
    }

    /// What the board does, unasked, when it comes up.
    ///
    /// The file was read in `init`; this acts on what it said. The radio comes on only if the switch
    /// was on, and one connection is attempted only if the file says to connect without being asked.
    /// A board with no file does nothing at all — the radio stays down until a finger turns it on,
    /// and the first network is chosen by hand from the page.
    ///
    /// Called from a thread of its own by `rust_main`, so the panel is up before the radio is.
    fn autoconnect(&mut self) -> Result<(), HalError> {
        let Some(saved) = self.saved() else {
            return Ok(());
        };

        if !saved.enabled {
            return Ok(());
        }

        self.set_enabled(true)?;

        if !saved.autoconnect {
            return Ok(());
        }

        eprintln!(
            "[wifi] boot: connecting to {:?}, because the file says autoconnect",
            saved.ssid
        );
        self.connect(&saved.ssid, &saved.password)
    }

    fn saved(&self) -> Option<WifiCredentials> {
        SAVED.lock().unwrap().clone()
    }

    fn remember(&mut self, credentials: &WifiCredentials) -> Result<(), HalError> {
        credentials
            .save(wifi_credentials::BOARD_APP_DATA)
            .map_err(io_error)?;

        eprintln!("[wifi] remembered {:?}", credentials.ssid);
        *SAVED.lock().unwrap() = Some(credentials.clone());
        Ok(())
    }

    fn forget(&mut self) -> Result<(), HalError> {
        WifiCredentials::forget(wifi_credentials::BOARD_APP_DATA).map_err(io_error)?;

        eprintln!("[wifi] forgot the remembered network");
        *SAVED.lock().unwrap() = None;
        Ok(())
    }

    /// The switch, and nothing else.
    ///
    /// It does not connect and it does not write. Connecting here would mean the radio came up and
    /// immediately reached for a network nobody asked for — coming up and connecting are
    /// `autoconnect`'s job, and it is the only path to either. Writing here would put the file back
    /// in the driver's hands, which is the thing the file exists to stop.
    fn set_enabled(&mut self, on: bool) -> Result<(), HalError> {
        HalError::from_code(unsafe { ffi::hal_wifi_set_enabled(on) })
    }

    #[inline]
    fn is_enabled(&self) -> bool {
        unsafe { ffi::hal_wifi_is_enabled() }
    }

    fn scan_start(&mut self) -> Result<(), HalError> {
        HalError::from_code(unsafe { ffi::hal_wifi_scan_start() })
    }

    #[inline]
    fn scan_state(&self) -> ScanState {
        match unsafe { ffi::hal_wifi_scan_get_state() } {
            SCAN_STATE_SCANNING => ScanState::Scanning,
            SCAN_STATE_DONE => ScanState::Done,
            SCAN_STATE_ERROR => ScanState::Error,
            _ => ScanState::Idle,
        }
    }

    fn scan_results(&self) -> Result<Vec<ApInfo>, HalError> {
        if self.scan_state() != ScanState::Done {
            return Err(HalError::Busy);
        }

        let mut records = [ffi::HalWifiAp {
            ssid: [0; ffi::SSID_MAX_LEN],
            rssi: 0,
            channel: 0,
            secure: false,
        }; ffi::MAX_AP_RECORDS];
        let mut count: u16 = 0;

        let ret = unsafe {
            ffi::hal_wifi_scan_get_results(
                records.as_mut_ptr(),
                ffi::MAX_AP_RECORDS as u16,
                &mut count,
            )
        };
        if ret != 0 {
            return Err(HalError::Internal(ret));
        }

        let mut aps = Vec::with_capacity(count as usize);
        for rec in records.iter().take(count as usize) {
            aps.push(ApInfo::new(
                c_buf_to_string(&rec.ssid),
                rec.rssi,
                rec.secure,
                rec.channel,
            ));
        }
        Ok(aps)
    }

    fn connect(&mut self, ssid: &str, password: &str) -> Result<(), HalError> {
        let c_ssid = CString::new(ssid).map_err(|_| HalError::InvalidArg)?;
        let c_pwd = CString::new(password).map_err(|_| HalError::InvalidArg)?;

        // Nothing is written here. The file belongs to the app: it is the app that holds the
        // password, and it is the app that hears the connection come up, so `remember` is called
        // from there. Writing at the *attempt* would mean a mistyped password destroys the one that
        // worked — which is exactly what the NVS copy this replaces did.
        HalError::from_code(unsafe { ffi::hal_wifi_connect(c_ssid.as_ptr(), c_pwd.as_ptr()) })
    }

    fn disconnect(&mut self) -> Result<(), HalError> {
        HalError::from_code(unsafe { ffi::hal_wifi_disconnect() })
    }

    fn status(&self) -> WifiStatus {
        let mut st = ffi::HalWifiStatus {
            state: 0,
            ssid: [0; ffi::SSID_MAX_LEN],
            ip: [0; ffi::IP_MAX_LEN],
            netmask: [0; ffi::IP_MAX_LEN],
            gateway: [0; ffi::IP_MAX_LEN],
            rssi: 0,
        };
        if unsafe { ffi::hal_wifi_get_status(&mut st) } != 0 {
            return WifiStatus::default();
        }

        WifiStatus {
            state: match st.state {
                STATE_SCANNING => WifiState::Scanning,
                STATE_CONNECTING => WifiState::Connecting,
                STATE_CONNECTED => WifiState::Connected,
                _ => WifiState::Disconnected,
            },
            ssid: c_buf_to_string(&st.ssid),
            ip: c_buf_to_string(&st.ip),
            netmask: c_buf_to_string(&st.netmask),
            gateway: c_buf_to_string(&st.gateway),
            rssi: st.rssi,
        }
    }
}
