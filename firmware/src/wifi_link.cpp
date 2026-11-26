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

namespace {
WebServer server(80);
String lastStatus = "{}";
String lastAlert = "{}";
bool wasConnected = false;
bool serverStarted = false;
unsigned long lastLog = 0;

void handleStatus() {
  String body = "{\"status\":" + lastStatus + ",\"alert\":" + lastAlert +
                ",\"wifi\":{\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"rssi\":" + String(WiFi.RSSI()) + "}}";
  server.sendHeader("Access-Control-Allow-Origin", "*");  // dashboard on localhost
  server.send(200, "application/json", body);
}
}  // namespace

void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("fallguard");
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("# Wi-Fi: connecting...");
  server.on("/status", handleStatus);
  server.onNotFound([] { server.send(404, "text/plain", "try /status\n"); });
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
    Serial.println("# Wi-Fi: http://fallguard.local/status");
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

#else  // no wifi_secrets.h

void wifiBegin() {
  Serial.println("# Wi-Fi disabled: copy wifi_secrets.example.h to wifi_secrets.h");
}
void wifiLoop() {}
void wifiSetStatus(const char*) {}
void wifiSetAlert(const char*) {}

#endif
