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
    Sd,
    Http,
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
#[derive(Debug, Clone, Copy)]
pub struct HalHttpEvent {
    pub status_code: i32,
    pub bytes: usize,
    pub error_code: i32,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub union HalEventData {
    pub button: HalButtonEvent,
    pub power: HalPowerEvent,
    pub wifi: HalWifiEvent,
    pub sd: i32,
    pub http: HalHttpEvent,
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
        pub fn hal_display_is_active() -> bool;
        pub fn hal_display_set_power(on: bool);
        pub fn hal_power_consume_wake_button() -> bool;
    }
}

impl HalEvent {
    pub fn to_system_event(self, board: &Board) -> Option<SystemEvent> {
        match self.event_type {
            HalEventType::Button => {
                let btn = unsafe { self.data.button };
                match btn {
                    HalButtonEvent::BootPress => Some(SystemEvent::InputAction(InputAction::Back)),
                    HalButtonEvent::SidePress => Some(SystemEvent::InputAction(InputAction::Exit)),
                    HalButtonEvent::PwrPress => Some(SystemEvent::InputAction(InputAction::Home)),
                    _ => None,
                }
            }
            HalEventType::Power => {
                let pwr = unsafe { self.data.power };
                match pwr.state {
                    HalPowerState::PekeyShort | HalPowerState::PekeyLong => {
                        Some(SystemEvent::InputAction(InputAction::Home))
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
            HalEventType::Sd => {
                let status = board.storage().sd_status();
                Some(SystemEvent::SdCardChanged(status))
            }
            HalEventType::Http => {
                let http = unsafe { self.data.http };
                Some(SystemEvent::HttpFinished {
                    status_code: if http.status_code > 0 { http.status_code as u16 } else { 0 },
                    bytes: http.bytes,
                    error_code: if http.error_code != 0 { Some(http.error_code) } else { None },
                })
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
            let mut last_second = u64::MAX;
            let mut last_minute = u32::MAX;
            loop {
                // Blocks in FreeRTOS xQueueReceive with 0% CPU!
                // Wakes instantly on any hardware interrupt, or every 500ms to check clock & wakeups.
                let has_event = unsafe { ffi::hal_event_wait(&mut ev, 500) };
                if has_event {
                    println!("[event_pump] HW event: type={:?}", ev.event_type);
                    let display_active = unsafe { ffi::hal_display_is_active() };
                    let wake_button_consumed = unsafe { ffi::hal_power_consume_wake_button() };
                    let mut swallow_event = false;
                    if !display_active || wake_button_consumed {
                        let is_user_wake_action = match ev.event_type {
                            HalEventType::Button => {
                                let btn = unsafe { ev.data.button };
                                matches!(
                                    btn,
                                    HalButtonEvent::BootPress
                                        | HalButtonEvent::SidePress
                                        | HalButtonEvent::PwrPress
                                )
                            }
                            HalEventType::Power => {
                                let pwr = unsafe { ev.data.power };
                                matches!(
                                    pwr.state,
                                    HalPowerState::PekeyShort | HalPowerState::PekeyLong
                                )
                            }
                            _ => false,
                        };

                        if is_user_wake_action {
                            println!("[event_pump] User wake action detected, turning display ON");
                            // Turn display back on!
                            unsafe { ffi::hal_display_set_power(true) };
                            // Swallow wake-up button press to prevent accidental UI triggers
                            swallow_event = true;
                        }
                    }

                    if !swallow_event {
                        if let Some(sys_ev) = ev.to_system_event(&board) {
                            board.emit_event(sys_ev);
                        }
                    }
                }

                // Check second & minute rollover for system-wide clock updates
                if let Ok(duration) =
                    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH)
                {
                    let total_secs = duration.as_secs();
                    if total_secs != last_second {
                        last_second = total_secs;
                        board.emit_event(SystemEvent::SecondTick { unix_secs: total_secs });

                        let current_minute = (total_secs / 60) as u32;
                        if current_minute != last_minute {
                            println!(
                                "[event_pump] Minute rollover: last={} -> current={} (epoch={})",
                                last_minute, current_minute, total_secs
                            );
                            last_minute = current_minute;
                            board.emit_event(SystemEvent::MinuteTick { unix_secs: total_secs });
                        }
                    }
                }

                // Check and dispatch scheduled system wakeup events (e.g. alarm clock)
                for req in board.wakeup_scheduler().poll_expired() {
                    println!(
                        "[event_pump] Wakeup triggered: id='{}', wake_screen={}",
                        req.id, req.wake_screen
                    );
                    if req.wake_screen && !board.is_display_on() {
                        let _ = board.set_display_power(true);
                    }
                    board.emit_event(SystemEvent::WakeupTriggered {
                        id: req.id,
                        wake_screen: req.wake_screen,
                    });
                }
            }
        })
        .expect("failed to spawn hal_event_pump");
}
