#pragma once
// Wi-Fi station mode: joins the network in wifi_secrets.h (git-ignored),
// reconnects automatically, and serves
//   GET http://fallguard.local/status  (or http://<ip>/status)
//   -> {"status":{...latest status JSON...},"alert":{...latest alert JSON...},
//       "wifi":{"ssid":"...","ip":"...","rssi":-55}}
// BLE alerting is unaffected; Wi-Fi is an extra channel.

void wifiBegin();
void wifiLoop();                       // call every loop iteration
void wifiSetStatus(const char* json);  // latest status JSON (from the BLE path)
void wifiSetAlert(const char* json);   // latest alert JSON
