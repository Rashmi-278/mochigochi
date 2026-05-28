// mpu6050.cpp — minimal MPU-6050 driver, software-I2C edition.
//
// The C3 has only one hardware I2C controller (already used by the OLED),
// so the MPU sits on a bit-banged I2C bus on PIN_MPU_SDA / PIN_MPU_SCL.
// The MPU module's on-board pull-ups hold the lines HIGH; we drive LOW
// by switching pinMode to OUTPUT+LOW, and "release" by switching back
// to INPUT (line floats HIGH via the pull-up). Standard open-drain.
//
// Init sequence on the MPU itself:
//   1. WHO_AM_I (0x75) must read back 0x68 — otherwise it's not an
//      MPU-6050 (some clones report 0x70 or 0x98; we accept anything
//      non-zero and proceed, the registers behave the same).
//   2. PWR_MGMT_1 (0x6B) ← 0x80 to reset, wait 100 ms.
//   3. PWR_MGMT_1 (0x6B) ← 0x01 to clear SLEEP and select the PLL with
//      gyro X clock (more stable than the internal 8 MHz osc).
//   4. CONFIG (0x1A) ← 0x03 — 44 Hz DLPF, kills accel noise.
//   5. ACCEL_CONFIG (0x1C) ← 0x08 (±4 g, 8192 LSB/g).
//   6. GYRO_CONFIG  (0x1B) ← 0x08 (±500 °/s, 65.5 LSB/(°/s)).

#include "mpu6050.h"

#include <Arduino.h>

#include "../config.h"

namespace {

// Register map (only the ones we touch).
constexpr uint8_t REG_SMPLRT_DIV = 0x19;
constexpr uint8_t REG_CONFIG = 0x1A;
constexpr uint8_t REG_GYRO_CONFIG = 0x1B;
constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;
constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;  // first byte of the 14-byte burst
constexpr uint8_t REG_PWR_MGMT_1 = 0x6B;
constexpr uint8_t REG_WHO_AM_I = 0x75;

constexpr float ACCEL_LSB_PER_G = 8192.0f;
constexpr float GYRO_LSB_PER_DPS = 65.5f;

bool ready = false;
bool pinsUp = false;

// --- Bit-banged I2C ---------------------------------------------------
// One bit at ~100 kHz = 10 µs. We split that across the SCL low/high
// halves; 4 µs per half is conservative on a 160 MHz C3 with the
// Arduino digitalWrite/pinMode overhead.
constexpr uint32_t HALF_BIT_US = 4;

inline void sclHigh() { pinMode(PIN_MPU_SCL, INPUT); }
inline void sclLow() {
  pinMode(PIN_MPU_SCL, OUTPUT);
  digitalWrite(PIN_MPU_SCL, LOW);
}
inline void sdaHigh() { pinMode(PIN_MPU_SDA, INPUT); }
inline void sdaLow() {
  pinMode(PIN_MPU_SDA, OUTPUT);
  digitalWrite(PIN_MPU_SDA, LOW);
}
inline bool sdaRead() { return digitalRead(PIN_MPU_SDA) == HIGH; }

void busBringUp() {
  if (pinsUp) return;
  // Start in the released state — both lines floating HIGH via the
  // module's pull-ups.
  sdaHigh();
  sclHigh();
  delayMicroseconds(HALF_BIT_US * 2);
  pinsUp = true;
}

void i2cStart() {
  // SDA goes LOW while SCL is HIGH = START condition.
  sdaHigh();
  sclHigh();
  delayMicroseconds(HALF_BIT_US);
  sdaLow();
  delayMicroseconds(HALF_BIT_US);
  sclLow();
  delayMicroseconds(HALF_BIT_US);
}

void i2cStop() {
  // SDA goes HIGH while SCL is HIGH = STOP condition.
  sdaLow();
  delayMicroseconds(HALF_BIT_US);
  sclHigh();
  delayMicroseconds(HALF_BIT_US);
  sdaHigh();
  delayMicroseconds(HALF_BIT_US);
}

// Returns true if the slave ACKed (pulled SDA low during the 9th bit).
bool i2cWriteByte(uint8_t b) {
  for (int i = 7; i >= 0; --i) {
    if (b & (1u << i))
      sdaHigh();
    else
      sdaLow();
    delayMicroseconds(HALF_BIT_US);
    sclHigh();
    delayMicroseconds(HALF_BIT_US);
    sclLow();
  }
  // ACK bit: release SDA, clock once, sample.
  sdaHigh();
  delayMicroseconds(HALF_BIT_US);
  sclHigh();
  delayMicroseconds(HALF_BIT_US);
  bool ack = !sdaRead();
  sclLow();
  delayMicroseconds(HALF_BIT_US);
  return ack;
}

uint8_t i2cReadByte(bool sendAck) {
  uint8_t b = 0;
  sdaHigh();  // release SDA so the slave drives it
  for (int i = 7; i >= 0; --i) {
    delayMicroseconds(HALF_BIT_US);
    sclHigh();
    delayMicroseconds(HALF_BIT_US);
    if (sdaRead()) b |= (1u << i);
    sclLow();
  }
  // Send ACK (LOW) to keep reading; NACK (HIGH) to stop after this byte.
  if (sendAck)
    sdaLow();
  else
    sdaHigh();
  delayMicroseconds(HALF_BIT_US);
  sclHigh();
  delayMicroseconds(HALF_BIT_US);
  sclLow();
  delayMicroseconds(HALF_BIT_US);
  sdaHigh();
  return b;
}

// --- Register-level helpers -------------------------------------------

bool writeReg(uint8_t reg, uint8_t value) {
  i2cStart();
  if (!i2cWriteByte((MPU_ADDR << 1) | 0)) {
    i2cStop();
    return false;
  }
  if (!i2cWriteByte(reg)) {
    i2cStop();
    return false;
  }
  if (!i2cWriteByte(value)) {
    i2cStop();
    return false;
  }
  i2cStop();
  return true;
}

bool readReg(uint8_t reg, uint8_t* out) {
  i2cStart();
  if (!i2cWriteByte((MPU_ADDR << 1) | 0)) {
    i2cStop();
    return false;
  }
  if (!i2cWriteByte(reg)) {
    i2cStop();
    return false;
  }
  i2cStart();  // repeated start
  if (!i2cWriteByte((MPU_ADDR << 1) | 1)) {
    i2cStop();
    return false;
  }
  *out = i2cReadByte(/*sendAck=*/false);  // single byte → NACK
  i2cStop();
  return true;
}

bool readBurst(uint8_t reg, uint8_t* buf, size_t n) {
  i2cStart();
  if (!i2cWriteByte((MPU_ADDR << 1) | 0)) {
    i2cStop();
    return false;
  }
  if (!i2cWriteByte(reg)) {
    i2cStop();
    return false;
  }
  i2cStart();
  if (!i2cWriteByte((MPU_ADDR << 1) | 1)) {
    i2cStop();
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    buf[i] = i2cReadByte(/*sendAck=*/i < n - 1);  // ACK all except the last
  }
  i2cStop();
  return true;
}

}  // namespace

namespace imu {

bool begin() {
  busBringUp();

  uint8_t who = 0;
  if (!readReg(REG_WHO_AM_I, &who)) {
    ready = false;
    return false;
  }
  // Real MPU-6050 reports 0x68; clones sometimes report 0x70/0x98.
  // Accept anything non-zero — the register layout is identical.
  if (who == 0x00 || who == 0xFF) {
    ready = false;
    return false;
  }

  if (!writeReg(REG_PWR_MGMT_1, 0x80)) {
    ready = false;
    return false;
  }  // reset
  delay(100);
  if (!writeReg(REG_PWR_MGMT_1, 0x01)) {
    ready = false;
    return false;
  }  // wake + PLL
  if (!writeReg(REG_CONFIG, 0x03)) {
    ready = false;
    return false;
  }  // 44 Hz DLPF
  if (!writeReg(REG_SMPLRT_DIV, 0x04)) {
    ready = false;
    return false;
  }  // 200 Hz
  if (!writeReg(REG_GYRO_CONFIG, 0x08)) {
    ready = false;
    return false;
  }
  if (!writeReg(REG_ACCEL_CONFIG, 0x08)) {
    ready = false;
    return false;
  }

  ready = true;
  return true;
}

bool isReady() { return ready; }

bool read(Sample& out) {
  if (!ready) return false;

  uint8_t buf[14];
  if (!readBurst(REG_ACCEL_XOUT_H, buf, sizeof(buf))) return false;

  int16_t ax = static_cast<int16_t>((buf[0] << 8) | buf[1]);
  int16_t ay = static_cast<int16_t>((buf[2] << 8) | buf[3]);
  int16_t az = static_cast<int16_t>((buf[4] << 8) | buf[5]);
  // buf[6..7] is temperature — skipped.
  int16_t gx = static_cast<int16_t>((buf[8] << 8) | buf[9]);
  int16_t gy = static_cast<int16_t>((buf[10] << 8) | buf[11]);
  int16_t gz = static_cast<int16_t>((buf[12] << 8) | buf[13]);

  out.accel.x = ax / ACCEL_LSB_PER_G;
  out.accel.y = ay / ACCEL_LSB_PER_G;
  out.accel.z = az / ACCEL_LSB_PER_G;
  out.gyro.x = gx / GYRO_LSB_PER_DPS;
  out.gyro.y = gy / GYRO_LSB_PER_DPS;
  out.gyro.z = gz / GYRO_LSB_PER_DPS;
  return true;
}

}  // namespace imu
