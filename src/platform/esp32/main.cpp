// VRChat OSC -> relay bridge, ESP32 firmware.
//
// VRChat finds this board through OSCQuery (or the launch option --osc=9000:<ESP32_IP>:9001) and
// sends it avatar parameters over UDP. The shared core (src/core) turns them into relay states.
// This file only provides WiFi, sockets, mDNS and the GPIO pins.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <stdarg.h>

#include "bridge.h"
#include "config.h"
#include "hal.h"

namespace hal {

uint32_t millis() { return ::millis(); }
void pinOutput(uint8_t pin) { pinMode(pin, OUTPUT); }
void pinWrite(uint8_t pin, bool high) { digitalWrite(pin, high ? HIGH : LOW); }

void log(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

}  // namespace hal

namespace {

WiFiUDP udp;
WebServer web(80);
WebServer oscqueryWeb(OSCQUERY_HTTP_PORT);

uint8_t rxBuf[1536];
IPAddress vrchatAddr;
bool wifiUp = false;
bool serversStarted = false;
String oscqueryName;

void sendToVrchat(const uint8_t* data, size_t len) {
  udp.beginPacket(vrchatAddr, VRCHAT_IN_PORT);
  udp.write(data, len);
  udp.endPacket();
}

void pollOsc() {
  int size;
  while ((size = udp.parsePacket()) > 0) {
    int n = udp.read(rxBuf, sizeof rxBuf);
    vrchatAddr = udp.remoteIP();
    bool ok = n > 0 && static_cast<size_t>(size) <= sizeof rxBuf;
    bridge::handlePacket(rxBuf, ok ? n : 0, vrchatAddr.toString().c_str());
  }
}

// Hands a request from the Arduino WebServer to one of the shared core's handlers.
void serve(WebServer& server, bridge::HttpResponse (*handler)(const bridge::HttpRequest&)) {
  bridge::HttpRequest req;
  req.method = server.method() == HTTP_POST ? "POST" : server.method() == HTTP_GET ? "GET" : "OTHER";
  req.path = server.uri().c_str();
  for (int i = 0; i < server.args(); i++) req.args[server.argName(i).c_str()] = server.arg(i).c_str();
  req.clientIp = server.client().remoteIP().toString().c_str();
  bridge::HttpResponse res = handler(req);
  server.send(res.status, res.contentType, res.body.c_str());
}

void startServers() {
  web.onNotFound([] { serve(web, bridge::handleWeb); });
  web.begin();
  if (ENABLE_OSCQUERY) {
    oscqueryWeb.onNotFound([] { serve(oscqueryWeb, bridge::handleOscQuery); });
    oscqueryWeb.begin();
  }
}

void onWifiUp() {
  udp.stop();
  udp.begin(OSC_LISTEN_PORT);
  if (!serversStarted) {
    startServers();
    serversStarted = true;
  }

  String ip = WiFi.localIP().toString();
  bridge::setDeviceInfo({"ESP32", ip.c_str(), OSC_LISTEN_PORT, ENABLE_OSCQUERY ? oscqueryName.c_str() : ""});

  MDNS.end();
  if (MDNS.begin(DEVICE_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    if (ENABLE_OSCQUERY) {
      MDNS.setInstanceName(oscqueryName.c_str());
      MDNS.addService("_oscjson", "_tcp", OSCQUERY_HTTP_PORT);
      MDNS.addService("_osc", "_udp", OSC_LISTEN_PORT);
      hal::log("[oscquery] advertising \"%s\" (HTTP %u, OSC UDP %u)\n", oscqueryName.c_str(),
               OSCQUERY_HTTP_PORT, OSC_LISTEN_PORT);
    }
  } else {
    hal::log("[wifi] mDNS failed to start - OSCQuery discovery won't work\n");
  }

  hal::log("[wifi] connected, IP %s (RSSI %d dBm)\n", ip.c_str(), WiFi.RSSI());
  hal::log("[wifi] status page: http://%s.local/  or  http://%s/\n", DEVICE_HOSTNAME, ip.c_str());
  if (ENABLE_OSCQUERY) {
    hal::log("[wifi] VRChat should find this board by itself (HUD: \"sending data to ...\")\n");
    hal::log("[wifi] fallback VRChat launch option: --osc=9000:%s:%u\n", ip.c_str(), OSC_LISTEN_PORT);
  } else {
    hal::log("[wifi] VRChat launch option: --osc=9000:%s:%u\n", ip.c_str(), OSC_LISTEN_PORT);
  }
}

void checkWifi() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up == wifiUp) return;
  wifiUp = up;
  if (up) {
    onWifiUp();
  } else {
    bridge::allOff("WiFi disconnected, reconnecting");
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== VRChat OSC relay bridge ===");

  bridge::begin(sendToVrchat);

  WiFi.mode(WIFI_STA);

  // Unique per board, so several ESP32s can coexist. VRChat shows this name in its HUD.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[8];
  snprintf(suffix, sizeof suffix, "-%02X%02X%02X", mac[3], mac[4], mac[5]);
  oscqueryName = String(OSCQUERY_NAME) + suffix;

  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.setSleep(false);  // modem sleep adds 100ms+ of latency to incoming UDP
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  hal::log("[wifi] connecting to \"%s\"...\n", WIFI_SSID);
}

void loop() {
  checkWifi();
  if (wifiUp) {
    pollOsc();
    web.handleClient();
    if (ENABLE_OSCQUERY) oscqueryWeb.handleClient();
  }
  bridge::update();
  delay(1);
}
