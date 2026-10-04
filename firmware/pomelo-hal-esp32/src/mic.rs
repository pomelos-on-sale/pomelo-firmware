//! Microphone backend (ES7210 ADC / I2S RX).
//!
//! P4 will add a private `mod ffi` here declaring the `hal_mic_*` bindings and
//! an I2S RX capture task.

use pomelo_hal::{HalError, MicBackend};

pub struct EspMic;

impl EspMic {
    pub const fn new() -> Self {
        Self
    }
}

impl Default for EspMic {
    fn default() -> Self {
        Self::new()
    }
}

impl MicBackend for EspMic {
    fn record_start(&mut self) -> Result<(), HalError> {
        // TODO(P4): wrap `hal_mic_record_start()`.
        Err(HalError::NotSupported)
    }

    fn read(&mut self, _buf: &mut [i16]) -> Result<usize, HalError> {
        // TODO(P4): wrap `hal_mic_read()`.
        Err(HalError::NotSupported)
    }

    fn record_stop(&mut self) -> Result<(), HalError> {
        // TODO(P4): wrap `hal_mic_record_stop()`.
        Err(HalError::NotSupported)
    }

    fn is_recording(&self) -> bool {
        false
    }
}
