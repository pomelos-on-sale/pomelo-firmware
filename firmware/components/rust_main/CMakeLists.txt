idf_component_register(
    INCLUDE_DIRS "include"
    # The C shims this crate calls (`hal_display_*`, `hal_wifi_*`) are `board_hal`'s. It is
    # listed here for intent, not for link order — what actually gets those objects
    # into the image is that component's `WHOLE_ARCHIVE`.
    REQUIRES board_hal
)

set(RUST_TARGET "xtensa-esp32s3-espidf")

# `esp198` (rustc 1.98.1), not `esp` (1.97): the older toolchain's Xtensa LLVM backend cannot
# compile `iced_core` at all ("Cannot select: ... XtensaISD::PCREL_WRAPPER" in
# `iced_core::theme::palette::Secondary::generate`, reproduced at opt-level 3 and "s"). It is a
# codegen bug in that toolchain, not in iced. Switching invalidates the cached `-Zbuild-std`
# artifacts below, so the first build after this is a cold one.

# A project of its own: cargo run from here uses this crate's `[workspace]`, and the patch table and
# release profile that decide how the image is built are in this crate's manifest. Pinning the
# target directory is what keeps `RUST_LIB` where it has always been — since the crate is its own
# root it is also cargo's default, but saying it out loud means invoking cargo from elsewhere cannot
# move the artifact, and it reuses the cached `-Zbuild-std` standard library either way (rebuilding
# that from scratch takes a quarter of an hour).
set(RUST_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/target")
set(RUST_LIB "${RUST_TARGET_DIR}/${RUST_TARGET}/release/librust_main.a")

# Extra cargo features for the Rust half. Empty by default (the same image as
# before); a cache variable rather than an environment read, because ESP-IDF only
# re-runs the configure step when a cache entry changes — an env var would look
# like it worked and silently not:
#
#   idf.py -DPOMELO_RUST_FEATURES=render-debug build flash monitor
#
# `render-debug` prints frame timings and highlights dirty rectangles.
set(POMELO_RUST_FEATURES "" CACHE STRING "Extra cargo features for the Rust component")
set(RUST_FEATURE_ARGS "")
if(POMELO_RUST_FEATURES)
    list(APPEND RUST_FEATURE_ARGS "--features" "${POMELO_RUST_FEATURES}")
endif()

add_custom_target(cargo_build_rust_main ALL
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${RUST_TARGET_DIR}"
            cargo +esp198 build -p rust_main --target ${RUST_TARGET} -Zbuild-std=std,panic_abort --release ${RUST_FEATURE_ARGS}
    BYPRODUCTS ${RUST_LIB}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Compiling Rust component (librust_main.a) for ESP32-S3 Xtensa..."
)

add_dependencies(${COMPONENT_LIB} cargo_build_rust_main)
target_link_libraries(${COMPONENT_LIB} INTERFACE ${RUST_LIB})
