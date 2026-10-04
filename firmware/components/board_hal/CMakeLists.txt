# The C side of the Rust FFI boundary.
#
# `board_hal.c` and its sub-modules define the `hal_*` shims (display, touch, buttons, audio, wifi, power);
# `pomelo-hal-esp32` and `rust_main` call them, which is to say `librust_main.a` does.
#
# Hence `WHOLE_ARCHIVE`: the linker walks static archives in link order and once,
# and ESP-IDF does not put the component archives in a group (only the C++ runtime
# libs and a couple of managed components get one). Leaving this as part of `main`
# put `libmain.a` *before* `librust_main.a`, so nothing had asked for `hal_wifi_*`
# when that archive was read and its members were never pulled in — the link then
# died on `undefined reference to hal_wifi_scan_start`. Naming this component in
# `rust_main`'s `REQUIRES` does not move it later (measured: it still came first),
# so the objects are forced in instead; the definitions are then present before
# anything can ask for them, whatever order IDF chooses.
idf_component_register(
    SRCS
        "board_hal.c"
        "board_display.c"
        "board_touch.c"
        "board_button.c"
        "board_power.c"
        "board_audio.c"
        "board_wifi.c"
    INCLUDE_DIRS "."
    WHOLE_ARCHIVE
    REQUIRES
        waveshare__esp32_s3_touch_amoled_2_16
        espressif__esp_codec_dev
        espressif__esp_lcd_co5300
        esp_lcd
        esp_lcd_touch
        driver
        vfs
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        esp_timer
        esp_rom
)
