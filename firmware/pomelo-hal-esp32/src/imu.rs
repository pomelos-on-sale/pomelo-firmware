//! QMI8658 IMU backend (accelerometer / gyroscope).
//!
//! P4 will add a private `mod ffi` here declaring the `hal_imu_*` bindings and
//! an I2C polling helper.

use pomelo_hal::{HalError, ImuBackend, Vec3};

pub struct EspImu;

impl EspImu {
    pub const fn new() -> Self {
        Self
    }
}

impl Default for EspImu {
    fn default() -> Self {
        Self::new()
    }
}

impl ImuBackend for EspImu {
    fn read_accel(&self) -> Result<Vec3, HalError> {
        // TODO(P4): wrap `hal_imu_read_accel()`.
        Err(HalError::NotSupported)
    }

    fn read_gyro(&self) -> Result<Vec3, HalError> {
        // TODO(P4): wrap `hal_imu_read_gyro()`.
        Err(HalError::NotSupported)
    }

    fn temperature_c(&self) -> Result<f32, HalError> {
        // TODO(P4): wrap `hal_imu_read_temperature()`.
        Err(HalError::NotSupported)
    }
}
