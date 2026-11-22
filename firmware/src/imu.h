#pragma once

struct ImuSample {
  float ax, ay, az;  // g
  float gx, gy, gz;  // deg/s
};

// Configures the MPU-6050: +-16 g, +-2000 dps, 21 Hz DLPF.
bool imuBegin();
bool imuRead(ImuSample& s);
