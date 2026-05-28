// mpu6050.h — minimal MPU-6050 driver (accel + gyro over shared I2C bus).
//
// The MPU-6050 sits on the same I2C bus as the OLED (PIN_SDA / PIN_SCL).
// We never call Wire.begin() here — U8g2 already owns the bus and has
// configured the pins. We just do byte-level reads/writes against
// MPU_ADDR.
//
// Reported values are in physical units:
//   accel — g (gravities). Stationary: |a| ≈ 1.0 g.
//   gyro  — °/s.
//
// Range is fixed at ±4 g / ±500 °/s — wide enough for a desk pet to be
// picked up and shaken without clipping, narrow enough to keep noise low.
#pragma once

#include <stdint.h>

namespace imu {

struct Vec3 {
  float x;
  float y;
  float z;
};

struct Sample {
  Vec3 accel;  // g
  Vec3 gyro;   // °/s
};

// True if WHO_AM_I matched and the chip is now out of sleep. Safe to call
// repeatedly — re-init on each call is idempotent.
bool begin();

// Did begin() succeed at least once? If false, read() returns junk.
bool isReady();

// Read one fresh accel + gyro sample. Returns false on I2C error. On
// failure the previous sample is left in `out` untouched.
bool read(Sample& out);

}  // namespace imu
