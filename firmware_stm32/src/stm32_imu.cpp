#include "stm32_imu.h"

#include <Adafruit_MPU6050.h>
#include <Wire.h>

#include "stm32_config.h"

namespace {
Adafruit_MPU6050 mpu;
constexpr float kMs2ToG = 1.0f / 9.80665f;
constexpr float kRadToDeg = 57.29578f;
}  // namespace

bool imuBegin() {
  Wire.setSDA(PIN_SDA);
  Wire.setSCL(PIN_SCL);
  Wire.begin();
  Wire.setClock(400000);
  if (!mpu.begin(MPU6050_I2CADDR_DEFAULT, &Wire)) return false;
  mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
  mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
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
