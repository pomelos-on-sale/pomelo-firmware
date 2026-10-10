//! The ESP32-S3 implementation of [`pomelo_hal`] — the HAL **hardware** layer.
//!
//! Raw `extern "C"` bindings live in a private `ffi` module inside each domain file,
//! immediately above the safe `Esp*` wrapper that consumes them, so the raw and safe code for
//! one peripheral stay together — and now also next to the C side, `components/board_hal/`,
//! which is where those signatures are written.
//!
//! [`board`] is the composition root's entry point: it assembles this board's backends into
//! the shared handle the launcher takes. Nothing else in the tree names an implementation —
//! the host tests build `pomelo_hal::Board::simulated()` instead, and app code only ever
//! sees the traits.
//!
//! P0 provides the module skeleton only: the constructors exist so [`board`] can assemble,
//! while data-producing calls return [`pomelo_hal::HalError::NotSupported`] until each
//! subsystem lands in a later phase:
//!
//! * `PowerBackend`  -> P1 (`hal_power_*`)
//! * `AudioBackend`  -> P2 (`hal_audio_*`)
//! * `WifiBackend`   -> P3 (`hal_wifi_*`)
//! * `MicBackend` / `ImuBackend` -> P4 (`hal_mic_*` / `hal_imu_*`)
//!
//! One file per hardware domain, mirroring `pomelo_hal::traits` and `pomelo_hal::sim`.
//!
//! This is a workspace member but **not** a default member, and the host test command
//! excludes it: its `extern "C"` functions exist only in the firmware image, so a host
//! binary that linked it would fail to link. `cargo check --workspace` still type-checks it
//! on any toolchain, which is what the old `_native-check` feature was for.
//!
//! It is deliberately **not** under `firmware/components/`. That directory is ESP-IDF's:
//! `idf.py` globs it and treats every subdirectory as a candidate component, so a directory
//! without a `CMakeLists.txt` gets a note printed on every configure. This crate is built by
//! cargo and referenced by `rust_main`; nothing in CMake has any reason to know it exists.
//! `components/rust_main` is the other way round — it *is* an IDF component, because CMake
//! builds its staticlib and links it into the image. The two directories encode exactly that
//! difference: `components/` = built by IDF, `firmware/<crate>/` = built by cargo.

mod audio;
mod event;
mod imu;
mod input;
mod mic;
mod power;
mod time;
mod wifi;

use std::sync::Arc;

use pomelo_hal::Board;

pub use audio::EspAudio;
pub use imu::EspImu;
pub use input::EspInput;
pub use mic::EspMic;
pub use power::EspPower;
pub use time::EspTime;
pub use wifi::EspWifi;

/// Assemble this board's backends into the shared handle the apps take.
///
/// Which implementation a board has is the composition root's decision, and this is the
/// device's half of it: `rust_main` builds [`board`] and hands it to the launcher, while a
/// desktop test builds `pomelo_hal::Board::simulated()`.
pub fn board() -> Arc<Board> {
    let board = Arc::new(Board::from_backends(
        Box::new(power::EspPower::new()),
        Box::new(wifi::EspWifi::new()),
        Box::new(audio::EspAudio::new()),
        Box::new(mic::EspMic::new()),
        Box::new(imu::EspImu::new()),
        Box::new(input::EspInput::new()),
        Box::new(time::EspTime::new()),
    ));
    event::start_event_pump(Arc::clone(&board));
    board
}
