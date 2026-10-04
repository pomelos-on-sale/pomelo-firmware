idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        board_hal
        rust_main
        joltwallet__littlefs
        vfs
)

target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=write" "-Wl,--wrap=esp_vfs_write" "-Wl,--wrap=_write_r")

littlefs_create_partition_image(internal "${CMAKE_SOURCE_DIR}/../../assets/internal" FLASH_IN_PROJECT)


