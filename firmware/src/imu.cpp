#include "imu.h"

#include <Arduino.h>
#include <Adafruit_MPU6050.h>
#include <Wire.h>

#include "config.h"

namespace {
Adafruit_MPU6050 mpu;
constexpr float kMs2ToG = 1.0f / 9.80665f;
constexpr float kRadToDeg = 57.29578f;
}  // namespace

// Prints every address that ACKs on the bus (helps diagnose wiring).
static void i2cScan() {
  Serial.print("# I2C scan:");
  int found = 0;
  for (uint8_t a = 1; a < 127; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); ++found; }
  }
  Serial.println(found ? "" : " no devices (check VCC/GND/SDA/SCL)");
}

bool imuBegin() {
  static bool wireStarted = false;
  if (!wireStarted) {
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);
    wireStarted = true;
  }
  // 0x68 with AD0 low/floating, 0x69 with AD0 high.
  if (!mpu.begin(0x68, &Wire) && !mpu.begin(0x69, &Wire)) {
    i2cScan();
    return false;
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);   // falls can exceed 8 g
  mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);      // anti-alias for 50 Hz
  return true;
}

bool imuRead(ImuSample& s) {
  sensors_event_t a, g, t;
  if (!mpu.getEvent(&a, &g, &t)) return false;
  s.ax = a.acceleration.x * kMs2ToG;
  s.ay = a.acceleration.y * kMs2ToG;
  s.az = a.acceleration.z * kMs2ToG;
  s.gx = g.gyro.x * kRadToDeg;
  s.gy = g.gyro.y * kRadToDeg;
  s.gz = g.gyro.z * kRadToDeg;
  return true;
}
