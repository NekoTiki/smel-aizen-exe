#pragma once
// Minimal OSCQuery service so VRChat discovers this board by itself and sends it avatar
// parameters, without the --osc launch option. VRChat browses mDNS for _oscjson._tcp, reads
// HOST_INFO to learn our OSC IP/port, and starts sending once our address tree contains /avatar.

#include <Arduino.h>

namespace oscquery {

// Starts the OSCQuery HTTP server. Call once, after WiFi is up.
void begin(const char* serviceName, uint16_t httpPort, uint16_t oscPort);

// Publishes the mDNS services. Call after every MDNS.begin().
void advertise();

// Call from loop().
void handleClient();

// Who last read our HOST_INFO (normally VRChat), for diagnostics. Empty if nobody yet.
const String& lastClient();
uint32_t lastQueryAt();

}  // namespace oscquery
