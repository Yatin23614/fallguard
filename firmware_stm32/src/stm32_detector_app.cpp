// On-device fall detection + BLE alerting.
//
// Alert flow:  confirmed fall -> PRE_ALERT (15 s beeping, short press cancels)
//              -> ALERTED (BLE notify, re-sent every 5 s until the phone acks
//                 or the wearer short-presses "I'm OK").
//              Holding the button 3 s at any time sends a manual SOS.
//
// Serial parity mode (used by ml/parity_check.py): send 128 lines
//   X,ax,ay,az,gx,gy,gz   -> replies  P,p_fall,p_sit,p_lie,p_other,us
#include <Arduino.h>

#include "stm32_app.h"
#include "stm32_ble_hm10.h"
#include "stm32_config.h"
#include "stm32_fall_detector.h"
#include "stm32_imu.h"
#include "stm32_model.h"
#include "stm32_norm_params.h"

namespace {
enum class Alert : uint8_t { Idle, PreAlert, Alerted };
enum class Btn : uint8_t { None, Short, Long };

Detector det;
Alert alertState = Alert::Idle;
FallEvent pending{};
unsigned long nextUs, preAlertStart, lastResend, lastStatus;
uint32_t alertId = 0;
char alertJson[200];

// ---------------------------------------------------------------- button
Btn pollButton(unsigned long now) {
  static bool down = false, longFired = false;
  static unsigned long downAt = 0, lastEdge = 0;
  const bool pressed = digitalRead(PIN_BUTTON) == LOW;
  if (now - lastEdge < 30) return Btn::None;  // debounce
  if (pressed && !down) {
    down = true; longFired = false; downAt = lastEdge = now;
  } else if (pressed && down && !longFired && now - downAt >= LONG_PRESS_MS) {
    longFired = true;
    return Btn::Long;
  } else if (!pressed && down) {
    down = false; lastEdge = now;
    if (!longFired) return Btn::Short;
  }
  return Btn::None;
}

// ---------------------------------------------------------------- outputs
void updateBuzzer(unsigned long now) {
  bool on = false;
  if (alertState == Alert::PreAlert) on = (now - preAlertStart) % 1000 < 150;
  else if (alertState == Alert::Alerted) on = now % 2000 < 500;
  digitalWrite(PIN_BUZZER, on);
  digitalWrite(PIN_LED, alertState != Alert::Idle ? on : (now % 3000 < 50));
}

int batteryMv() {
  if (PIN_BATT_ADC < 0) return -1;
  return (int)(analogRead(PIN_BATT_ADC) * 3300L / 1023) * 2;  // 10-bit, 1:2 divider
}

void sendStatus() {
  static const char* names[] = {"idle", "pre_alert", "alerted"};
  char buf[96];
  snprintf(buf, sizeof buf, "{\"state\":\"%s\",\"uptime_s\":%lu,\"batt_mv\":%d}",
           names[(int)alertState], millis() / 1000, batteryMv());
  bleSendStatus(buf);
}

void raiseAlert(const char* type, const FallEvent* ev) {
  ++alertId;
  if (ev) {
    snprintf(alertJson, sizeof alertJson,
             "{\"id\":%lu,\"type\":\"%s\",\"conf\":%.2f,\"angle\":%.0f,"
             "\"still\":%.2f,\"model\":%s,\"uptime_s\":%lu}",
             (unsigned long)alertId, type, ev->cls.probs[kFallIdx], ev->angleDeg,
             ev->stillFrac, ev->cls.hasModel ? "true" : "false", millis() / 1000);
  } else {
    snprintf(alertJson, sizeof alertJson,
             "{\"id\":%lu,\"type\":\"%s\",\"uptime_s\":%lu}",
             (unsigned long)alertId, type, millis() / 1000);
  }
  alertState = Alert::Alerted;
  lastResend = millis();
  bleSendAlert(alertJson);
  Serial.printf("# ALERT %s (BLE %s)\n", alertJson,
                bleConnected() ? "sent" : "queued, no client");
}

void clearAlert(const char* why) {
  Serial.printf("# alert cleared: %s\n", why);
  if (alertState == Alert::Alerted) {
    // Overwrite the characteristic so a receiver that (re)connects later
    // does not mistake the old alert for an active one.
    char buf[64];
    snprintf(buf, sizeof buf, "{\"id\":%lu,\"type\":\"%s\"}",
             (unsigned long)alertId, strcmp(why, "ack") == 0 ? "acked" : "cancel");
    bleSendAlert(buf);
  }
  alertState = Alert::Idle;
}

// ---------------------------------------------------------------- parity
void pollParity() {
  static char line[128];
  static int len = 0, rows = 0;
  static float win[kWin][kChannels];
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n') {
      if (len < (int)sizeof line - 1) line[len++] = c;
      continue;
    }
    line[len] = 0;
    len = 0;
    float v[6];
    if (sscanf(line, "X,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4],
               &v[5]) != 6)
      continue;
    for (int k = 0; k < 6; ++k) win[rows][k] = v[k];
    if (++rows < kWin) continue;
    rows = 0;
    float p[kNumClasses] = {0};
    unsigned long t0 = micros();
    bool ok = modelPredict(win, p);
    unsigned long us = micros() - t0;
    Serial.print("P");
    for (int k = 0; k < kNumClasses; ++k) Serial.printf(",%.4f", ok ? p[k] : -1.0f);
    Serial.printf(",%lu\n", us);
  }
}
}  // namespace

void appSetup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  while (!imuBegin()) {
    Serial.println("# MPU-6050 not found, check wiring (SDA=D14/PB9, SCL=D15/PB8)");
    delay(1000);
  }
  modelInit();
  bleBegin(BLE_DEVICE_NAME);
  Serial.printf("# FallGuard ready (model: %s)\n", modelAvailable() ? "yes" : "NO");
  nextUs = micros();
  lastStatus = millis();
}

void appLoop() {
  const unsigned long now = millis();
  pollParity();

  // ---- 50 Hz sampling + detection
  if ((long)(micros() - nextUs) >= 0) {
    nextUs += SAMPLE_PERIOD_US;
    ImuSample s;
    FallEvent ev;
    if (imuRead(s) && det.push(s, ev)) {
      Serial.printf("# FALL confirmed: p=%.2f angle=%.0f still=%.2f\n",
                    ev.cls.probs[kFallIdx], ev.angleDeg, ev.stillFrac);
      if (alertState == Alert::Idle) {
        pending = ev;
        preAlertStart = now;
        alertState = Alert::PreAlert;
      }
    }
    Classification c;
    if (det.takeClassification(c)) {
      Serial.print("# impact -> ");
      for (int k = 0; k < kNumClasses; ++k)
        Serial.printf("%s=%.2f ", kClassNames[k], c.probs[k]);
      Serial.println(c.fallLike ? "(checking post-impact)" : "(ignored)");
    }
  }

  // ---- button / BLE / alert state machine
  const Btn b = pollButton(now);
  if (b == Btn::Long && alertState != Alert::Alerted) {
    raiseAlert("sos", nullptr);
  } else if (b == Btn::Short && alertState != Alert::Idle) {
    clearAlert("button");
  }
  if (bleTakeAck() && alertState == Alert::Alerted) clearAlert("ack");

  if (alertState == Alert::PreAlert && now - preAlertStart >= PRE_ALERT_MS) {
    raiseAlert("fall", &pending);
  }
  if (alertState == Alert::Alerted && now - lastResend >= ALERT_RESEND_MS) {
    lastResend = now;
    if (bleConnected()) bleSendAlert(alertJson);
  }
  if (now - lastStatus >= STATUS_PERIOD_MS) {
    lastStatus = now;
    sendStatus();
  }
  updateBuzzer(now);
}
