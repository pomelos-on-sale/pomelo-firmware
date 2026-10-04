# pomelo-firmware

The ESP32-S3 firmware and hardware abstraction host for [Pomelo OS](https://github.com/pomelos-on-sale).

## 📖 Overview

- **Platform**: Waveshare ESP32-S3-Touch-AMOLED-2.16 (Xtensa Dual-Core LX7 @ 240MHz, 8MB PSRAM, 16MB Flash)
- **Framework**: Built on ESP-IDF v6.1 and Rust Xtensa Toolchain (`esp198`)
- **Graphics & UI**: Runs on top of `pomelo-gfx` (Direct-to-RGB565) and `iced-pomelo-winit`

## 🚀 Building & Flashing

```bash
cd firmware

# Ensure ESP-IDF and Rust Xtensa toolchain are installed
./flash.sh
```
