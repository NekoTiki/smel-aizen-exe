#include "oscquery.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

namespace oscquery {
namespace {

// ACCESS: 0 = none, 1 = read, 2 = write, 3 = read/write. We only receive, so /avatar is write-only.
const char kChangeNode[] = R"({"FULL_PATH":"/avatar/change","ACCESS":2,"TYPE":"s"})";
const char kAvatarNode[] =
    R"({"FULL_PATH":"/avatar","ACCESS":2,"CONTENTS":{"change":{"FULL_PATH":"/avatar/change","ACCESS":2,"TYPE":"s"}}})";
const char kRootNode[] =
    R"({"DESCRIPTION":"root node","FULL_PATH":"/","ACCESS":0,"CONTENTS":{"avatar":)"
    R"({"FULL_PATH":"/avatar","ACCESS":2,"CONTENTS":{"change":{"FULL_PATH":"/avatar/change","ACCESS":2,"TYPE":"s"}}}}})";

WebServer* server = nullptr;
String name;
uint16_t httpPort = 0;
uint16_t oscPort = 0;
String client;
uint32_t queriedAt = 0;

void sendHostInfo() {
  client = server->client().remoteIP().toString();
  queriedAt = millis();
  String json = "{\"NAME\":\"" + name + "\",\"OSC_IP\":\"" + WiFi.localIP().toString() +
                "\",\"OSC_PORT\":" + String(oscPort) +
                ",\"OSC_TRANSPORT\":\"UDP\",\"EXTENSIONS\":{\"ACCESS\":true,\"CLIPMODE\":false,"
                "\"RANGE\":true,\"TYPE\":true,\"VALUE\":true}}";
  server->send(200, "application/json", json);
}

// OSCQuery serves the node at the requested path; HOST_INFO is a query flag on any path.
void handleRequest() {
  if (server->hasArg("HOST_INFO")) return sendHostInfo();

  String uri = server->uri();
  if (uri.length() > 1 && uri.endsWith("/")) uri.remove(uri.length() - 1);
  if (uri == "/") return server->send(200, "application/json", kRootNode);
  if (uri == "/avatar") return server->send(200, "application/json", kAvatarNode);
  if (uri == "/avatar/change") return server->send(200, "application/json", kChangeNode);
  server->send(404, "application/json", "{}");
}

}  // namespace

void begin(const char* serviceName, uint16_t http, uint16_t osc) {
  if (server) return;
  // Unique per board, so several ESP32s can coexist. VRChat shows this name in its HUD.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[8];
  snprintf(suffix, sizeof suffix, "-%02X%02X%02X", mac[3], mac[4], mac[5]);
  name = String(serviceName) + suffix;
  httpPort = http;
  oscPort = osc;

  server = new WebServer(httpPort);
  server->onNotFound(handleRequest);
  server->begin();
}

void advertise() {
  MDNS.setInstanceName(name.c_str());
  MDNS.addService("_oscjson", "_tcp", httpPort);
  MDNS.addService("_osc", "_udp", oscPort);
  Serial.printf("[oscquery] advertising \"%s\" (HTTP %u, OSC UDP %u)\n", name.c_str(), httpPort, oscPort);
}

void handleClient() {
  if (server) server->handleClient();
}

const String& lastClient() { return client; }
uint32_t lastQueryAt() { return queriedAt; }

}  // namespace oscquery
