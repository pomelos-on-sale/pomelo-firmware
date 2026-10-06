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

    // The radio comes up, and maybe connects, on a thread of its own. Boot is for the panel: a
    // connection is a thing that happens *later*, and a board that waited for one would show nothing
    // for as long as the network took. No file, or a switch left off, and this thread does nothing
    // at all.
    {
        let board = Arc::clone(&board);

        std::thread::Builder::new()
            .name("wifi_autoconnect".into())
            .spawn(move || {
                if let Err(error) = board.wifi().autoconnect() {
                    eprintln!("[wifi] autoconnect: {error:?}");
                }
            })
            .expect("failed to spawn wifi_autoconnect");
    }

    println!("[iced] starting app-launcher on AMOLED panel (480x480)");

    if let Err(error) = app_launcher::program(Arc::clone(&board)).run() {
        eprintln!("[iced] the application stopped: {error:?}");
    }
}
