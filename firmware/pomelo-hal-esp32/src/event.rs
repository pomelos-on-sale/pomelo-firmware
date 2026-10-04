//! Unified hardware interrupt-to-event pump for ESP32-S3.
//!
//! Reads events from `board_hal`'s FreeRTOS queue (`hal_event_wait`) with 0% CPU
//! when idle, and dispatches them directly to `pomelo_hal::Board::emit_event`.

use std::sync::Arc;
use pomelo_hal::{Board, InputAction, SystemEvent};
use crate::input::HalButtonEvent;

#[repr(C)]
#[allow(dead_code)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HalEventType {
    None = 0,
    Button,
    Power,
    Wifi,
}

#[repr(C)]
#[allow(dead_code)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HalPowerState {
    ChargingStarted = 1,
    ChargingStopped,
    BatteryUpdate,
    PekeyShort,
    PekeyLong,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct HalPowerEvent {
    pub state: HalPowerState,
    pub percent: i32,
    pub voltage_mv: i32,
    pub is_charging: bool,
}

#[repr(C)]
#[allow(dead_code)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HalWifiEventType {
    Connected = 1,
    Disconnected,
    ScanDone,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct HalWifiEvent {
    pub event_type: HalWifiEventType,
    pub status: i32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union HalEventData {
    pub button: HalButtonEvent,
    pub power: HalPowerEvent,
    pub wifi: HalWifiEvent,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct HalEvent {
    pub event_type: HalEventType,
    pub data: HalEventData,
}

impl Default for HalEvent {
    fn default() -> Self {
        Self {
            event_type: HalEventType::None,
            data: HalEventData {
                button: HalButtonEvent::None,
            },
        }
    }
}

mod ffi {
    use super::HalEvent;

    extern "C" {
        pub fn hal_event_wait(out_event: *mut HalEvent, timeout_ms: u32) -> bool;
    }
}

impl HalEvent {
    pub fn to_system_event(&self, board: &Board) -> Option<SystemEvent> {
        match self.event_type {
            HalEventType::Button => {
                let btn = unsafe { self.data.button };
                match btn {
                    HalButtonEvent::BootPress => Some(SystemEvent::InputAction(InputAction::Back)),
                    HalButtonEvent::SidePress | HalButtonEvent::PwrPress => {
                        Some(SystemEvent::InputAction(InputAction::Exit))
                    }
                    _ => None,
                }
            }
            HalEventType::Power => {
                let pwr = unsafe { self.data.power };
                match pwr.state {
                    HalPowerState::PekeyShort | HalPowerState::PekeyLong => {
                        Some(SystemEvent::InputAction(InputAction::Exit))
                    }
                    HalPowerState::ChargingStarted
                    | HalPowerState::ChargingStopped
                    | HalPowerState::BatteryUpdate => {
                        let percent = if pwr.percent >= 0 {
                            pwr.percent.min(100) as u8
                        } else {
                            board.power().battery_percent().unwrap_or(0)
                        };
                        let voltage_mv = if pwr.voltage_mv >= 0 {
                            pwr.voltage_mv as u32
                        } else {
                            board.power().battery_voltage_mv().unwrap_or(0)
                        };
                        Some(SystemEvent::BatteryChanged {
                            percent,
                            charging: pwr.is_charging,
                            voltage_mv,
                        })
                    }
                }
            }
            HalEventType::Wifi => {
                let status = board.wifi().status();
                Some(SystemEvent::WifiStatusChanged(status))
            }
            HalEventType::None => None,
        }
    }
}

/// Spawns the background event pump thread.
///
/// Blocks on `hal_event_wait` (0% CPU via FreeRTOS queue) and
/// dispatches `SystemEvent` instances directly into `board.emit_event()`.
pub fn start_event_pump(board: Arc<Board>) {
    std::thread::Builder::new()
        .name("hal_event_pump".into())
        .spawn(move || {
            let mut ev = HalEvent::default();
            let mut last_minute = u32::MAX;
            loop {
                // Blocks in FreeRTOS xQueueReceive with 0% CPU!
                // Wakes instantly on any hardware interrupt, or every 1 second to check clock.
                if unsafe { ffi::hal_event_wait(&mut ev, 1000) } {
                    if let Some(sys_ev) = ev.to_system_event(&board) {
                        board.emit_event(sys_ev);
                    }
                }

                // Check minute rollover for system-wide clock updates
                if let Ok(duration) =
                    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)
                {
                    let total_secs = duration.as_secs();
                    let local_secs = total_secs + 28800; // UTC+8
                    let current_minute = ((local_secs % 86400) / 60) as u32;
                    if current_minute != last_minute {
                        last_minute = current_minute;
                        let percent = board.power().battery_percent().unwrap_or(0);
                        let charging = board.power().is_charging().unwrap_or(false);
                        let voltage_mv = board.power().battery_voltage_mv().unwrap_or(0);
                        board.emit_event(SystemEvent::BatteryChanged {
                            percent,
                            charging,
                            voltage_mv,
                        });
                    }
                }
            }
        })
        .expect("failed to spawn hal_event_pump");
}
