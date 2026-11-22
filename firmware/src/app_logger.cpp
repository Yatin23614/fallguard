// Data-collection firmware: streams t_ms,ax,ay,az,gx,gy,gz at 50 Hz.
// Read it with ml/record.py.
#include <Arduino.h>

#include "app.h"
#include "config.h"
#include "imu.h"

static unsigned long nextUs;

void appSetup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  while (!imuBegin()) {
    Serial.printf("# MPU-6050 not found, check wiring (SDA=%d, SCL=%d)\n", PIN_SDA, PIN_SCL);
    delay(1000);
  }
  Serial.println("# t_ms,ax,ay,az,gx,gy,gz");
  nextUs = micros();
}

void appLoop() {
  if ((long)(micros() - nextUs) < 0) return;
  nextUs += SAMPLE_PERIOD_US;
  ImuSample s;
  if (!imuRead(s)) return;
  Serial.printf("%lu,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n", millis(), s.ax, s.ay,
                s.az, s.gx, s.gy, s.gz);
  digitalWrite(PIN_LED, (millis() / 500) % 2);
}
