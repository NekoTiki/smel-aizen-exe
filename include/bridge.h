#pragma once
// Shared application core: everything between "a UDP packet arrived" and "a relay switched",
// plus the status page and the OSCQuery responses. The ESP32 firmware and the PC twin only
// wrap sockets, mDNS and GPIO around it, so both run exactly this code.

#include <map>
#include <string>

#include "relays.h"

namespace bridge {

struct HttpRequest {
  std::string method;                        // "GET", "POST", ...
  std::string path;                          // without the query string
  std::map<std::string, std::string> args;   // query arguments; bare flags like ?HOST_INFO map to ""
  std::string clientIp;
};

struct HttpResponse {
  HttpResponse(int status, const char* contentType, std::string body)
      : status(status), contentType(contentType), body(std::move(body)) {}
  int status;
  const char* contentType;
  std::string body;
};

struct DeviceInfo {
  std::string kind;          // shown on the status page: "ESP32", "PC twin"
  std::string ip;            // address VRChat should send OSC to
  uint16_t oscPort;
  std::string oscqueryName;  // empty when OSCQuery is off
};

// Sends raw OSC bytes to VRChat's input port. The platform knows VRChat's address.
using SendFn = void (*)(const uint8_t* data, size_t len);

void begin(SendFn sendToVrchat);
void setDeviceInfo(const DeviceInfo& info);

// A UDP packet arrived on the OSC port. Pass len = 0 for a packet that couldn't be read.
void handlePacket(const uint8_t* data, size_t len, const std::string& fromIp);

// Call every loop iteration: ends pulses and enforces maxOnMs.
void update();

// Opens every relay, e.g. when WiFi drops and we can no longer hear "contact released".
void allOff(const char* reason);

// Status page + JSON API (port 80 on the ESP32).
HttpResponse handleWeb(const HttpRequest& req);

// OSCQuery endpoints: HOST_INFO and the address tree (OSCQUERY_HTTP_PORT).
HttpResponse handleOscQuery(const HttpRequest& req);

}  // namespace bridge
