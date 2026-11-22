#pragma once
// BLE alerts through an HM-10 BLE-UART module (the F401RE has no radio).
// Same function names as the ESP32 build's ble_alert.h, so the detector app
// logic is identical. The HM-10 exposes service 0xFFE0 / characteristic 0xFFE1;
// messages are newline-terminated JSON:
//   alert : {"id":..,"type":"fall|sos|cancel|acked",...}
//   status: {"state":..,"uptime_s":..,"batt_mv":..}
// Anything the phone writes (e.g. "ACK") acknowledges the current alert.
// Receive with:  python receiver/ble_receiver.py --hm10

void bleBegin(const char* name);
bool bleConnected();
void bleSendAlert(const char* json);
void bleSendStatus(const char* json);
bool bleTakeAck();  // true once per received ack
