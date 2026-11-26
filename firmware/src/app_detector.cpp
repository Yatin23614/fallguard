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
#include <math.h>

#include "app.h"
#include "ble_alert.h"
#include "config.h"
#include "detector.h"
#include "imu.h"
#include "model.h"
#include "norm_params.h"
#include "wifi_link.h"

namespace {
enum class Alert : uint8_t { Idle, PreAlert, Alerted };
enum class Btn : uint8_t { None, Short, Long };

Detector det;
Alert alertState = Alert::Idle;
FallEvent pending{};
unsigned long nextUs, preAlertStart, lastResend, lastStatus;
uint32_t alertId = 0;
int motionDiv = 0;
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
  return analogReadMilliVolts(PIN_BATT_ADC) * 2;  // 1:2 divider
}

void sendStatus() {
  static const char* names[] = {"idle", "pre_alert", "alerted"};
  char buf[96];
  snprintf(buf, sizeof buf, "{\"state\":\"%s\",\"uptime_s\":%lu,\"batt_mv\":%d}",
           names[(int)alertState], millis() / 1000, batteryMv());
  bleSendStatus(buf);
  wifiSetStatus(buf);
}

// Every CNN run (fall-like or not) so the dashboard can show what it decided.
void sendImpact(const Classification& c) {
  char buf[160];
  int n = snprintf(buf, sizeof buf, "{\"event\":\"impact\",\"probs\":[");
  for (int k = 0; k < kNumClasses; ++k)
    n += snprintf(buf + n, sizeof buf - n, "%s%.3f", k ? "," : "", c.probs[k]);
  snprintf(buf + n, sizeof buf - n, "],\"fall_like\":%s,\"model\":%s,\"uptime_s\":%lu}",
           c.fallLike ? "true" : "false", c.hasModel ? "true" : "false",
           millis() / 1000);
  bleSendStatus(buf);
  wifiSetImpact(buf);
}

int16_t clamp16(float v) {
  return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : lroundf(v)));
}

void sendMotion(const ImuSample& s) {
  static uint8_t seq = 0;
  const int16_t v[6] = {clamp16(s.ax * 1000), clamp16(s.ay * 1000), clamp16(s.az * 1000),
                        clamp16(s.gx * 10), clamp16(s.gy * 10), clamp16(s.gz * 10)};
  uint8_t pkt[14];
  for (int i = 0; i < 6; ++i) {
    pkt[2 * i] = (uint8_t)(v[i] & 0xFF);
    pkt[2 * i + 1] = (uint8_t)((uint16_t)v[i] >> 8);
  }
  pkt[12] = (uint8_t)det.state();
  pkt[13] = seq++;
  if (bleConnected()) bleSendMotion(pkt, sizeof pkt);
  wifiPushMotion(v, pkt[12]);
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
  wifiSetAlert(alertJson);
  sendStatus();
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
    wifiSetAlert(buf);
  }
  alertState = Alert::Idle;
  sendStatus();
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
    Serial.printf("# MPU-6050 not found, check wiring (SDA=%d, SCL=%d)\n", PIN_SDA, PIN_SCL);
    delay(1000);
  }
  modelInit();
  bleBegin(BLE_DEVICE_NAME);
  wifiBegin();
  sendStatus();
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
    const bool ok = imuRead(s);
    if (ok && det.push(s, ev)) {
      Serial.printf("# FALL confirmed: p=%.2f angle=%.0f still=%.2f\n",
                    ev.cls.probs[kFallIdx], ev.angleDeg, ev.stillFrac);
      if (alertState == Alert::Idle) {
        pending = ev;
        preAlertStart = now;
        alertState = Alert::PreAlert;
        sendStatus();
      }
    }
    Classification c;
    if (det.takeClassification(c)) {
      Serial.print("# impact -> ");
      for (int k = 0; k < kNumClasses; ++k)
        Serial.printf("%s=%.2f ", kClassNames[k], c.probs[k]);
      Serial.println(c.fallLike ? "(checking post-impact)" : "(ignored)");
      sendImpact(c);
    }
    if (++motionDiv >= SAMPLE_HZ / 10) {  // 10 Hz live stream for the dashboard
      motionDiv = 0;
      if (ok) sendMotion(s);  // BLE (if connected) + Wi-Fi ring buffer
    }
  }

  // ---- button / BLE / alert state machine
  const Btn b = pollButton(now);
  if (b == Btn::Long && alertState != Alert::Alerted) {
    raiseAlert("sos", nullptr);
  } else if (b == Btn::Short && alertState != Alert::Idle) {
    clearAlert("button");
  }
  const bool acked = bleTakeAck() | wifiTakeAck();  // '|' so both flags are consumed
  if (acked && alertState == Alert::Alerted) clearAlert("ack");

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
  wifiLoop();
}
