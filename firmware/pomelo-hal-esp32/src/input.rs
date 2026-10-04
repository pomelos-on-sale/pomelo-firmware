use std::time::Duration;
use pomelo_hal::traits::InputBackend;
use pomelo_hal::types::InputAction;

#[repr(C)]
#[allow(dead_code)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HalButtonEvent {
    None = 0,
    BootPress,
    BootRelease,
    PwrPress,
    PwrRelease,
    SidePress,
    SideRelease,
}

mod ffi {
    use super::HalButtonEvent;

    extern "C" {
        pub fn hal_button_wait_event(out_event: *mut HalButtonEvent, timeout_ms: u32) -> bool;
    }
}

/// ESP32-S3 hardware input backend.
///
/// Maps physical button 1 (BOOT / GPIO 0) to [`InputAction::Back`].
/// Maps physical button 2 (PWR / PEK) and button 3 (GPIO 18) to [`InputAction::Exit`].
pub struct EspInput;

impl EspInput {
    pub const fn new() -> Self {
        Self
    }

    /// Blocks the current FreeRTOS task until an input event arrives or timeout expires.
    ///
    /// Consumes 0% CPU while blocked via `xQueueReceive`.
    pub fn wait_action(&mut self, timeout: Duration) -> Option<InputAction> {
        let mut ev = HalButtonEvent::None;
        let timeout_ms = timeout.as_millis().min(u32::MAX as u128) as u32;

        if unsafe { ffi::hal_button_wait_event(&mut ev, timeout_ms) } {
            match ev {
                HalButtonEvent::BootPress => Some(InputAction::Back),
                HalButtonEvent::SidePress | HalButtonEvent::PwrPress => Some(InputAction::Exit),
                _ => None,
            }
        } else {
            None
        }
    }
}

impl Default for EspInput {
    fn default() -> Self {
        Self::new()
    }
}

impl InputBackend for EspInput {
    #[inline]
    fn poll_action(&mut self) -> Option<InputAction> {
        // Non-blocking poll (timeout = 0)
        self.wait_action(Duration::ZERO)
    }
}
