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

  hal::log("[wifi] connected, IP %s (RSSI %d dBm)\n", ip.c_str(), WiFi.RSSI());

  // mDNS: what this board announces on the network. ESPmDNS applies the instance name to every
  // service, so all three show up under the same name.
  MDNS.end();
  bool mdnsUp = MDNS.begin(DEVICE_HOSTNAME);
  if (mdnsUp) {
    hal::log("[mdns] hostname  %s.local -> %s\n", DEVICE_HOSTNAME, ip.c_str());
    if (ENABLE_OSCQUERY) MDNS.setInstanceName(oscqueryName.c_str());
    const char* instance = ENABLE_OSCQUERY ? oscqueryName.c_str() : DEVICE_HOSTNAME;
    auto announce = [&](const char* service, const char* proto, uint16_t port, const char* what) {
      bool ok = MDNS.addService(service, proto, port);
      hal::log("[mdns] service   %s.%s  \"%s\" port %u  (%s)%s\n", service, proto, instance, port, what,
               ok ? "" : "  FAILED");
    };
    announce("_http", "_tcp", 80, "status page");
    if (ENABLE_OSCQUERY) {
      announce("_oscjson", "_tcp", OSCQUERY_HTTP_PORT, "OSCQuery, VRChat reads this");
      announce("_osc", "_udp", OSC_LISTEN_PORT, "OSC input");
    } else {
      hal::log("[mdns] OSCQuery off (ENABLE_OSCQUERY = false): VRChat won't find the board by itself\n");
    }
  } else {
    hal::log("[mdns] FAILED to start: no %s.local and no OSCQuery discovery\n", DEVICE_HOSTNAME);
  }

  if (mdnsUp) {
    hal::log("[web] status page  http://%s.local/  or  http://%s/\n", DEVICE_HOSTNAME, ip.c_str());
  } else {
    hal::log("[web] status page  http://%s/\n", ip.c_str());
  }
  hal::log("[osc] listening on UDP %u\n", OSC_LISTEN_PORT);
  if (ENABLE_OSCQUERY && mdnsUp) {
    hal::log("[vrchat] should find this board by itself (HUD: \"sending data to %s\")\n", oscqueryName.c_str());
    hal::log("[vrchat] fallback launch option: --osc=9000:%s:%u\n", ip.c_str(), OSC_LISTEN_PORT);
  } else {
    hal::log("[vrchat] launch option needed: --osc=9000:%s:%u\n", ip.c_str(), OSC_LISTEN_PORT);
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
