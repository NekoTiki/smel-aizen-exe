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

// ----- Live status page (Server-Sent Events) -----------------------------------------------------
// WebServer forgets a connection once the handler returns, but WiFiClient copies share the socket,
// so keeping a copy keeps the stream open. Sockets are scarce on the ESP32, hence the low cap.

constexpr size_t kMaxEventClients = 2;
WiFiClient eventClients[kMaxEventClients];
uint32_t eventClientSince[kMaxEventClients];
uint32_t lastEventCheck = 0;

size_t openEventClients() {
  size_t n = 0;
  for (WiFiClient& c : eventClients) n += c ? 1 : 0;
  return n;
}

void dropEventClient(size_t i, const char* why) {
  eventClients[i].stop();
  eventClients[i] = WiFiClient();
  hal::log("[web] live page %s (%u open)\n", why, static_cast<unsigned>(openEventClients()));
}

void dropAllEventClients() {
  for (size_t i = 0; i < kMaxEventClients; i++) {
    if (eventClients[i]) dropEventClient(i, "disconnected");
  }
}

// WiFiClient::write retries for up to 10 s on a full socket; a browser that went out of range
// must not freeze relay handling, so only write when the socket can take data right now.
bool writeNow(WiFiClient& c, const std::string& data) {
  int fd = c.fd();
  if (fd < 0) return false;
  fd_set set;
  FD_ZERO(&set);
  FD_SET(fd, &set);
  timeval zero = {0, 0};
  if (select(fd + 1, nullptr, &set, nullptr, &zero) <= 0) return false;
  return c.write(reinterpret_cast<const uint8_t*>(data.data()), data.size()) == data.size();
}

void handleEvents() {
  WiFiClient c = web.client();
  size_t slot = 0;
  for (size_t i = 0; i < kMaxEventClients; i++) {  // a free slot, else the oldest stream
    if (!eventClients[i]) {
      slot = i;
      break;
    }
    if (eventClientSince[i] < eventClientSince[slot]) slot = i;
  }
  if (eventClients[slot]) dropEventClient(slot, "dropped (too many open)");
  if (!writeNow(c, std::string(bridge::kEventStreamHeaders) + bridge::eventSnapshot())) return;
  eventClients[slot] = c;
  eventClientSince[slot] = millis();
  hal::log("[web] live page connected (%u open)\n", static_cast<unsigned>(openEventClients()));
}

void broadcastEvents() {
  std::string events = bridge::takeEvents(openEventClients() > 0);
  bool check = millis() - lastEventCheck >= 1000;
  if (check) lastEventCheck = millis();
  for (size_t i = 0; i < kMaxEventClients; i++) {
    WiFiClient& c = eventClients[i];
    if (!c) continue;
    if ((check && !c.connected()) || (!events.empty() && !writeNow(c, events))) dropEventClient(i, "disconnected");
  }
}

void startServers() {
  web.on(bridge::kEventsPath, HTTP_GET, handleEvents);
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
    dropAllEventClients();
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
  if (wifiUp) broadcastEvents();
  delay(1);
}
