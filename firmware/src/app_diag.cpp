// Wiring diagnostic (env:diag). The MPU-6050 breakout has pull-up resistors on
// SDA/SCL, so a pin wired to a *powered* module reads HIGH even with the
// ESP32's internal pull-down enabled; an unconnected pin reads LOW.
// Then tries an I2C scan on every plausible SDA/SCL pair.
#include <Arduino.h>
#include <Wire.h>

#include "app.h"

static const int kPins[] = {4, 5, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};

void appSetup() {
  Serial.begin(115200);
  delay(500);
}

void appLoop() {
  Serial.print("# pulled-up pins (wired to a powered module):");
  int high = 0;
  for (int p : kPins) {
    pinMode(p, INPUT_PULLDOWN);
    delay(2);
    if (digitalRead(p) == HIGH) { Serial.printf(" GPIO%d", p); ++high; }
  }
  Serial.println(high ? "" : " none");

  for (int sda : kPins) {
    for (int scl : kPins) {
      if (sda == scl) continue;
      pinMode(sda, INPUT_PULLDOWN); pinMode(scl, INPUT_PULLDOWN);
      delay(1);
      if (digitalRead(sda) != HIGH || digitalRead(scl) != HIGH) continue;
      Wire.begin(sda, scl, 100000);
      for (uint8_t a = 1; a < 127; ++a) {
        Wire.beginTransmission(a);
        if (Wire.endTransmission() == 0)
          Serial.printf("# FOUND device 0x%02X with SDA=GPIO%d SCL=GPIO%d\n", a, sda, scl);
      }
      Wire.end();
    }
  }
  Serial.println("# diag pass done");
  delay(3000);
}
