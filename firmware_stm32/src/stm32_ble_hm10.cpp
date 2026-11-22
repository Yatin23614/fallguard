// BLE via an HM-10 (CC2541/CC2640 "BLE UART") module -- the Nucleo-F401RE has
// no radio. Implements the same alert API (ble_alert.h) as the ESP32 build.
//
// The HM-10 exposes one service 0xFFE0 with one characteristic 0xFFE1
// (notify + write-without-response). Everything printed to its UART is
// notified to the phone in 20-byte chunks, so messages are newline-terminated
// JSON (receiver/ble_receiver.py --hm10 reassembles them):
//   alert : {"id":..,"type":"fall|sos|cancel|acked",...}\n
//   status: {"state":..,"uptime_s":..,"batt_mv":..}\n
// Anything the phone writes (e.g. "ACK") arrives on the UART = acknowledgement.
#include <Arduino.h>
#include <string.h>

#include "stm32_ble_hm10.h"
#include "stm32_config.h"

namespace {
// STM32duino core 4.x: the concrete UART class is `Uart`. Serial1 is not
// instantiated by default (needs HAVE_HWSERIAL1), so we own USART1 here.
Uart hm10((pin_size_t)PIN_BLE_RX, (pin_size_t)PIN_BLE_TX);
bool connKnown = false;  // module reported OK+CONN / OK+LOST at least once
bool connected = false;
bool ackFlag = false;
char rx[48];
int rxLen = 0;

bool endsWith(const char* s, int n, const char* suf) {
  const int m = strlen(suf);
  return n >= m && memcmp(s + n - m, suf, m) == 0;
}

// AT commands only work while no phone is connected. Genuine HM-10s want no
// line ending; clones (MLT-BT05, AT-09) want "\r\n" -- set HM10_AT_CRLF in
// stm32_config.h if the module's name does not change to FallGuard.
void at(const char* cmd) {
  hm10.print(cmd);
  if (HM10_AT_CRLF) hm10.print("\r\n");
  delay(300);
  while (hm10.available()) hm10.read();  // discard "OK..." replies
}

void poll() {
  while (hm10.available()) {
    const char c = hm10.read();
    if (rxLen < (int)sizeof rx - 1) rx[rxLen++] = c;
    rx[rxLen] = 0;
    if (endsWith(rx, rxLen, "OK+CONN")) {
      connKnown = connected = true;
      rxLen = 0;
    } else if (endsWith(rx, rxLen, "OK+LOST")) {
      connKnown = true;
      connected = false;
      rxLen = 0;
    } else if (c == '\n' || c == '\r' || endsWith(rx, rxLen, "ACK") ||
               endsWith(rx, rxLen, "ack")) {
      if (rxLen > 1 && strncmp(rx, "OK", 2) != 0) ackFlag = true;
      rxLen = 0;
    } else if (rxLen >= (int)sizeof rx - 1) {
      rxLen = 0;
    }
  }
}

void sendLine(const char* json) {
  hm10.print(json);
  hm10.print('\n');
}
}  // namespace

void bleBegin(const char* name) {
  hm10.begin(HM10_BAUD);
  delay(200);
  at("AT");
  char cmd[32];
  snprintf(cmd, sizeof cmd, "AT+NAME%s", name);
  at(cmd);
  at("AT+NOTI1");  // report OK+CONN / OK+LOST on the UART
  at("AT+RESET");  // apply the new name
  delay(500);
}

// If the module never reports connection events, assume a phone may be
// listening so alerts keep being re-sent.
bool bleConnected() {
  poll();
  return connKnown ? connected : true;
}

void bleSendAlert(const char* json) { sendLine(json); }
void bleSendStatus(const char* json) { sendLine(json); }

bool bleTakeAck() {
  poll();
  if (!ackFlag) return false;
  ackFlag = false;
  return true;
}
