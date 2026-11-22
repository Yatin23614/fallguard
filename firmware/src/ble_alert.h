#pragma once
// BLE GATT service "FallGuard":
//   alert  (read/notify): JSON {"id","type":"fall|sos|cancel","conf",...}
//   status (read/notify): JSON {"state","uptime_s","batt_mv"}
//                        also impact events {"event":"impact","probs":[..],..}
//   ack    (write)      : any value acknowledges the current alert
//   motion (notify)     : 10 Hz, 14 bytes little-endian:
//                         int16 ax,ay,az [mg]; int16 gx,gy,gz [0.1 deg/s];
//                         uint8 detector state; uint8 sequence
#include <stdint.h>

#define FG_SERVICE_UUID "6e3f0001-8b1a-4c2d-9a7e-1f2b3c4d5e6f"
#define FG_ALERT_UUID   "6e3f0002-8b1a-4c2d-9a7e-1f2b3c4d5e6f"
#define FG_STATUS_UUID  "6e3f0003-8b1a-4c2d-9a7e-1f2b3c4d5e6f"
#define FG_ACK_UUID     "6e3f0004-8b1a-4c2d-9a7e-1f2b3c4d5e6f"
#define FG_MOTION_UUID  "6e3f0005-8b1a-4c2d-9a7e-1f2b3c4d5e6f"

void bleBegin(const char* name);
bool bleConnected();
void bleSendAlert(const char* json);
void bleSendStatus(const char* json);
void bleSendMotion(const uint8_t* data, unsigned len);
bool bleTakeAck();  // true once per received ack
