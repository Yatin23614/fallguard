#pragma once
// Wi-Fi station mode: joins the network in wifi_secrets.h (git-ignored),
// reconnects automatically, and serves the dashboard plus a small JSON API:
//   GET  /                 dashboard (web/index.html, gzipped into the firmware)
//   GET  /live?since=SEQ   {"seq","uptime_s","m":[[ax,ay,az,gx,gy,gz,stage],..],
//                           "status":{..},"alert":{..},"impact":{..},"impact_n",
//                           "wifi":{"ip","rssi"}}   (m: mg and 0.1 deg/s, newer than SEQ)
//   GET  /status           {"status":{..},"alert":{..},"wifi":{..}}
//   POST /ack              acknowledge the active alert (same as the BLE ack)
// Reachable at http://fallguard.local/ or http://<ip>/ (printed over serial).
// BLE alerting is unaffected; Wi-Fi is an extra channel.
#include <stdint.h>

void wifiBegin();
void wifiLoop();                        // call every loop iteration
void wifiSetStatus(const char* json);   // latest status JSON
void wifiSetAlert(const char* json);    // latest alert JSON
void wifiSetImpact(const char* json);   // latest CNN impact JSON
void wifiPushMotion(const int16_t v[6], uint8_t stage);  // 10 Hz samples
bool wifiTakeAck();                     // true once per POST /ack
