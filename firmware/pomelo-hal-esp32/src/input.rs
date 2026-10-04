use pomelo_hal::traits::InputBackend;

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

/// ESP32-S3 hardware input backend.
///
/// Physical button interrupts (BOOT, PWR, SIDE) are pumped directly into
/// the unified hardware event queue via `hal_event_pump`.
pub struct EspInput;

impl EspInput {
    pub const fn new() -> Self {
        Self
    }
}

impl Default for EspInput {
    fn default() -> Self {
        Self::new()
    }
}

impl InputBackend for EspInput {}

