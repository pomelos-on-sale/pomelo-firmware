//! Rust firmware entrypoint.

use std::sync::Arc;

/// Native firmware entrypoint called by main.c
#[no_mangle]
pub extern "C" fn rust_main_entry() {
    println!("[POMELO UI] rust main entry started");

    std::panic::set_hook(Box::new(|info| {
        eprintln!("\n🚨 [RUST PANIC] {info}\n");
    }));

    let board = pomelo_hal_esp32::board();
    board.init();

    println!("[iced] starting app-launcher on AMOLED panel (480x480)");

    if let Err(error) = app_launcher::program(Arc::clone(&board)).run() {
        eprintln!("[iced] the application stopped: {error:?}");
    }
}
