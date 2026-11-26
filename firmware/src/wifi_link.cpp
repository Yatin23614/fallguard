#include "wifi_link.h"

#include <Arduino.h>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#define WIFI_CONFIGURED 1
#else
#define WIFI_CONFIGURED 0
#endif

#if WIFI_CONFIGURED
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "web_index.h"  // generated from web/index.html by embed_web.py

namespace {
WebServer server(80);
String lastStatus = "{}";
String lastAlert = "{}";
String lastImpact = "{}";
uint32_t impactCount = 0;
volatile bool ackFlag = false;
bool wasConnected = false;
bool serverStarted = false;
unsigned long lastLog = 0;

// Ring buffer of recent 10 Hz motion samples for /live.
struct MotionRec {
  int16_t v[6];
  uint8_t stage;
};
constexpr uint32_t kRing = 64;
MotionRec ring[kRing];
uint32_t motionSeq = 0;  // sequence number of the newest sample (0 = none yet)

void cors() { server.sendHeader("Access-Control-Allow-Origin", "*"); }

String wifiJson() {
  return "{\"ip\":\"" + WiFi.localIP().toString() + "\",\"rssi\":" + String(WiFi.RSSI()) + "}";
}

void handleRoot() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", reinterpret_cast<const char*>(kWebIndexGz), kWebIndexGzLen);
}

void handleStatus() {
  cors();
  server.send(200, "application/json",
              "{\"status\":" + lastStatus + ",\"alert\":" + lastAlert + ",\"wifi\":" + wifiJson() + "}");
}

void handleLive() {
  uint32_t since = server.hasArg("since") ? strtoul(server.arg("since").c_str(), nullptr, 10) : 0;
  // Unknown/too-old cursor: send the last 2 s.
  if (since > motionSeq || motionSeq - since > kRing - 4) since = motionSeq > 20 ? motionSeq - 20 : 0;
  String body;
  body.reserve(1400);
  body += "{\"seq\":" + String(motionSeq) + ",\"uptime_s\":" + String(millis() / 1000) + ",\"m\":[";
  for (uint32_t s = since + 1; s <= motionSeq; ++s) {
    const MotionRec& r = ring[s % kRing];
    if (s != since + 1) body += ',';
    body += '[';
    for (int i = 0; i < 6; ++i) { body += String(r.v[i]); body += ','; }
    body += String(r.stage);
    body += ']';
  }
  body += "],\"status\":" + lastStatus + ",\"alert\":" + lastAlert + ",\"impact\":" + lastImpact +
          ",\"impact_n\":" + String(impactCount) + ",\"wifi\":" + wifiJson() + "}";
  cors();
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleAck() {
  ackFlag = true;
  cors();
  server.send(200, "application/json", "{\"ok\":true}");
}
}  // namespace

void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("fallguard");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("# Wi-Fi: connecting...");
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/live", HTTP_GET, handleLive);
  server.on("/ack", HTTP_POST, handleAck);
  server.onNotFound([] { server.send(404, "text/plain", "not found\n"); });
}

void wifiLoop() {
  const bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wasConnected) {
    Serial.printf("# Wi-Fi: connected, IP %s, RSSI %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (!serverStarted) {
      server.begin();
      serverStarted = true;
    }
    MDNS.end();
    if (MDNS.begin("fallguard")) MDNS.addService("http", "tcp", 80);
    Serial.printf("# Wi-Fi: dashboard at http://fallguard.local/ or http://%s/\n",
                  WiFi.localIP().toString().c_str());
  } else if (!up && wasConnected) {
    Serial.println("# Wi-Fi: disconnected, retrying");
  } else if (!up && millis() - lastLog > 15000) {
    lastLog = millis();
    Serial.printf("# Wi-Fi: still connecting (status %d)\n", (int)WiFi.status());
  }
  wasConnected = up;
  if (serverStarted) server.handleClient();
}

void wifiSetStatus(const char* json) { lastStatus = json; }
void wifiSetAlert(const char* json) { lastAlert = json; }
void wifiSetImpact(const char* json) {
  lastImpact = json;
  ++impactCount;
}

void wifiPushMotion(const int16_t v[6], uint8_t stage) {
  ++motionSeq;
  MotionRec& r = ring[motionSeq % kRing];
  for (int i = 0; i < 6; ++i) r.v[i] = v[i];
  r.stage = stage;
}

bool wifiTakeAck() {
  if (!ackFlag) return false;
  ackFlag = false;
  return true;
}

#else  // no wifi_secrets.h

void wifiBegin() {
  Serial.println("# Wi-Fi disabled: copy wifi_secrets.example.h to wifi_secrets.h");
}
void wifiLoop() {}
void wifiSetStatus(const char*) {}
void wifiSetAlert(const char*) {}
void wifiSetImpact(const char*) {}
void wifiPushMotion(const int16_t[6], uint8_t) {}
bool wifiTakeAck() { return false; }

#endif
