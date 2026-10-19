#include <stdio.h>
#include <reent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_hal.h"
#include "rust_main.h"

static const char *TAG = "c_main";

// =============================================================================
// 【底层平台守护】：VFS / POSIX 标准输出静默丢弃封装 (__wrap_write / __wrap_esp_vfs_write)
// -----------------------------------------------------------------------------
// 背景与原理：
// ESP32-S3 原生 USB-Serial-JTAG 控制台在未插物理 USB 线（纯电池供电）时，
// ESP-IDF 底层驱动 (usb_serial_jtag_vfs.c) 在检测到 usb_serial_jtag_is_connected() == false
// 时会强制返回 -1 并设置 errno = EIO。
//
// C 语言的 printf / ESP_LOGI 不关心返回值，即便出错也只是丢弃。
// 但 Rust 标准库 std::io::stdout() 底层在调用 write(1, ...) 时，如果收到负数或 EIO，
// 其内部 _print() 会无条件触发 panic!("failed printing to stdout: ...")！
// 在嵌入式固件 release 模式 (panic = "abort") 下，panic 直接调用 C abort()，
// 导致 ESP32-S3 立即硬件复位重启，从而在电池模式下出现“拔线即死、无限重启”的幽灵 Bug！
//
// 解决方案：
// 通过 GCC 链接器参数 -Wl,--wrap=write, -Wl,--wrap=esp_vfs_write, -Wl,--wrap=_write_r
// 在链接期拦截所有指向标准库写入的调用：
// 1. 若目标是文件、Socket 等普通描述符 (fd >= 3)，保持原生行为与真实错误码返回不变。
// 2. 若目标是标准输出/标准错误 (fd == 1 或 fd == 2)，且底层驱动返回错误（如 USB 未连），
//    则伪装成功，直接返回请求的写入字节数 (count / size)。
// 这样在纯电池模式下，println! / eprintln! 的日志被当做“写入虚拟黑洞 (/dev/null)”静默吞下，
// Rust std 认为写入完全成功，绝不触发 panic，彻底在平台底层根治该问题！
// =============================================================================

extern ssize_t __real_write(int fd, const void *buf, size_t count);
extern ssize_t __real_esp_vfs_write(struct _reent *r, int fd, const void *data, size_t size);
extern ssize_t __real__write_r(struct _reent *r, int fd, const void *data, size_t size);

ssize_t __wrap_write(int fd, const void *buf, size_t count)
{
    ssize_t ret = __real_write(fd, buf, count);
    if ((fd == 1 || fd == 2) && ret < 0) {
        return (ssize_t)count;
    }
    return ret;
}

ssize_t __wrap_esp_vfs_write(struct _reent *r, int fd, const void *data, size_t size)
{
    ssize_t ret = __real_esp_vfs_write(r, fd, data, size);
    if ((fd == 1 || fd == 2) && ret < 0) {
        return (ssize_t)size;
    }
    return ret;
}

ssize_t __wrap__write_r(struct _reent *r, int fd, const void *data, size_t size)
{
    ssize_t ret = __real__write_r(r, fd, data, size);
    if ((fd == 1 || fd == 2) && ret < 0) {
        return (ssize_t)size;
    }
    return ret;
}

void app_main(void)
{
    // =========================================================================
    // 【关键防护】：设置 stdout / stderr 为完全非缓冲模式（_IONBF）
    // -------------------------------------------------------------------------
    // 本板使用 USB-Serial-JTAG 作为主控制台。在纯电池模式（无物理 USB 连接）下，
    // 配合 __wrap_write 确保输出安全静默丢弃，防止 Rust std 触发致命 panic。
    // =========================================================================
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "===============================================");
    ESP_LOGI(TAG, "  ESP-IDF Bootloader & Kernel Initialized");
    ESP_LOGI(TAG, "  Target: ESP32-S3 (Xtensa Dual-Core LX7)");
    ESP_LOGI(TAG, "  Two-Track Fast Boot Architecture Activated");
    ESP_LOGI(TAG, "===============================================");

    // Initialize display & core power (Track 1) and spawn background worker (Track 2)
    esp_err_t err = board_hal_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board_hal_init failed: %d", err);
    } else {
        ESP_LOGI(TAG, "Hardware core initialized successfully.");
    }

    ESP_LOGI(TAG, "Handing over to Rust GUI Shell...");

    // Call into native Rust code
    rust_main_entry();

    ESP_LOGI(TAG, "Rust main entry returned.");
}
