// VRChat OSC -> relay bridge.
//
// VRChat sends avatar parameter changes as OSC over UDP. It finds this board through OSCQuery
// (or the launch option --osc=9000:<ESP32_IP>:9001) and every Contact Receiver parameter listed
// in config.h switches a relay. A status page at http://vrc-relay.local/ shows every parameter seen.

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <map>

#include "config.h"
#include "osc.h"
#include "oscquery.h"
#include "relays.h"

namespace {

constexpr char kParamPrefix[] = "/avatar/parameters/";
constexpr size_t kParamPrefixLen = sizeof(kParamPrefix) - 1;
constexpr size_t kMaxTrackedParams = 128;

struct SeenParam {
  String value;
  uint32_t updatedAt;
};

WiFiUDP udp;
WebServer web(80);
RelayBank relays(RELAYS, sizeof(RELAYS) / sizeof(RELAYS[0]));

uint8_t rxBuf[1536];
uint8_t txBuf[256];

bool wifiUp = false;
bool webStarted = false;
IPAddress vrchatIp;
bool vrchatKnown = false;
uint32_t lastPacketAt = 0;
uint32_t packetCount = 0;
uint32_t malformedCount = 0;
String currentAvatar;
std::map<String, SeenParam> seenParams;  // everything VRChat reported, for the status page

String formatValue(const osc::Value& v) {
  switch (v.type) {
    case osc::Value::Type::Bool: return v.b ? "true" : "false";
    case osc::Value::Type::Int: return String(v.i);
    case osc::Value::Type::Float: return String(v.f, 3);
    case osc::Value::Type::String: return String(v.s);
    default: return "-";
  }
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 2);
  for (char c : in) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<uint8_t>(c) < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

// ----- OSC ---------------------------------------------------------------------------------------

void sendToVrchat(const char* address, const osc::Value& value) {
  if (!vrchatKnown) return;
  size_t n = osc::buildMessage(txBuf, sizeof txBuf, address, value);
  if (n == 0) return;
  udp.beginPacket(vrchatIp, VRCHAT_IN_PORT);
  udp.write(txBuf, n);
  udp.endPacket();
}

void onRelayChanged(size_t index, bool on) {
  const RelayConfig& cfg = relays.config(index);
  Serial.printf("[relay] %-20s GPIO%-2u -> %s\n", cfg.param, cfg.pin, on ? "ON" : "off");
  if (cfg.feedbackParam) {
    String address = String(kParamPrefix) + cfg.feedbackParam;
    sendToVrchat(address.c_str(), osc::Value::fromBool(on));
  }
}

void onOscMessage(const char* address, const osc::Value& value) {
  if (strncmp(address, kParamPrefix, kParamPrefixLen) == 0) {
    const char* name = address + kParamPrefixLen;
    auto it = seenParams.find(name);
    if (it != seenParams.end()) {
      it->second = {formatValue(value), millis()};
    } else if (seenParams.size() < kMaxTrackedParams) {
      seenParams[name] = {formatValue(value), millis()};
      Serial.printf("[osc] new parameter: %s = %s\n", name, formatValue(value).c_str());
    }

    bool mapped = relays.handleParameter(name, value.asFloat());
    if (LOG_ALL_PARAMS || mapped) {
      Serial.printf("[osc] %s = %s\n", name, formatValue(value).c_str());
    }
    return;
  }

  if (strcmp(address, "/avatar/change") == 0) {
    currentAvatar = value.type == osc::Value::Type::String ? value.s : "";
    Serial.printf("[osc] avatar changed: %s - all relays off\n", currentAvatar.c_str());
    relays.allOff();
    seenParams.clear();
    return;
  }

  if (LOG_ALL_PARAMS) Serial.printf("[osc] %s = %s\n", address, formatValue(value).c_str());
}

void pollOsc() {
  int size;
  while ((size = udp.parsePacket()) > 0) {
    int n = udp.read(rxBuf, sizeof rxBuf);
    vrchatIp = udp.remoteIP();
    vrchatKnown = true;
    lastPacketAt = millis();
    packetCount++;
    if (n <= 0 || static_cast<size_t>(size) > sizeof rxBuf) {
      malformedCount++;
      continue;
    }
    if (!osc::parsePacket(rxBuf, n, onOscMessage)) malformedCount++;
  }
}

// ----- Web status page ---------------------------------------------------------------------------

const char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>VRC Relay</title>
<style>
body{font-family:system-ui,sans-serif;margin:16px;background:#111;color:#eee}
table{border-collapse:collapse;width:100%;max-width:720px;margin-bottom:24px}
td,th{border-bottom:1px solid #333;padding:6px 8px;text-align:left;font-size:14px}
.on{color:#4ade80;font-weight:600}.off{color:#888}
button{background:#333;color:#eee;border:1px solid #555;border-radius:4px;padding:4px 10px;cursor:pointer}
code{background:#222;padding:2px 6px;border-radius:4px}#info{color:#aaa;font-size:14px}
</style></head><body>
<h2>VRChat OSC relays</h2>
<p id="info"></p>
<table><thead><tr><th>Relay parameter</th><th>GPIO</th><th>Mode</th><th>Input</th><th>Relay</th><th></th></tr></thead><tbody id="relays"></tbody></table>
<h3>Parameters from VRChat</h3>
<table><thead><tr><th>Name</th><th>Value</th><th>Age</th></tr></thead><tbody id="params"></tbody></table>
<script>
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
async function relay(i,on){await fetch(`/api/relay?i=${i}&on=${on?1:0}`,{method:'POST'});refresh();}
async function refresh(){
  const s=await (await fetch('/api/state')).json();
  document.getElementById('info').innerHTML=
    `ESP32 ${esc(s.ip)} &middot; ${s.oscquery?'OSCQuery on, fallback':'VRChat'} launch option: <code>--osc=9000:${esc(s.ip)}:${s.port}</code><br>`+
    (s.oscquery?(s.oscqueryClient?`OSCQuery: discovered by ${esc(s.oscqueryClient)} ${(s.oscqueryAgeMs/1000).toFixed(0)} s ago<br>`:'OSCQuery: not discovered yet<br>'):'')+
    (s.vrchat?`VRChat at ${esc(s.vrchat)}, last packet ${s.lastPacketMs} ms ago, ${s.packets} packets (${s.malformed} malformed)`:'No OSC traffic yet')+
    (s.avatar?`<br>Avatar ${esc(s.avatar)}`:'');
  document.getElementById('relays').innerHTML=s.relays.map((r,i)=>
    `<tr><td>${esc(r.param)}</td><td>${r.pin}</td><td>${r.mode}</td><td>${r.input?'active':'-'}</td>`+
    `<td class="${r.on?'on':'off'}">${r.on?'ON':'off'}</td>`+
    `<td>${s.webControl?`<button onclick="relay(${i},${!r.on})">${r.on?'Turn off':'Turn on'}</button>`:''}</td></tr>`).join('');
  document.getElementById('params').innerHTML=s.params.map(p=>
    `<tr><td>${esc(p.name)}</td><td>${esc(p.value)}</td><td>${(p.ageMs/1000).toFixed(1)} s</td></tr>`).join('');
}
refresh();setInterval(()=>refresh().catch(()=>{}),1000);
</script></body></html>)HTML";

const char* modeName(RelayMode mode) {
  switch (mode) {
    case RelayMode::Follow: return "follow";
    case RelayMode::Pulse: return "pulse";
    case RelayMode::Toggle: return "toggle";
  }
  return "?";
}

void handleState() {
  uint32_t now = millis();
  String json;
  json.reserve(512 + seenParams.size() * 64);
  json += "{\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"port\":" + String(OSC_LISTEN_PORT);
  json += ",\"vrchat\":" + (vrchatKnown ? "\"" + vrchatIp.toString() + "\"" : String("null"));
  json += ",\"lastPacketMs\":" + String(now - lastPacketAt);
  json += ",\"packets\":" + String(packetCount);
  json += ",\"malformed\":" + String(malformedCount);
  json += ",\"avatar\":\"" + jsonEscape(currentAvatar) + "\"";
  json += ",\"webControl\":" + String(ENABLE_WEB_CONTROL ? "true" : "false");
  json += ",\"oscquery\":" + String(ENABLE_OSCQUERY ? "true" : "false");
  json += ",\"oscqueryClient\":\"" + oscquery::lastClient() + "\"";
  json += ",\"oscqueryAgeMs\":" + String(now - oscquery::lastQueryAt());

  json += ",\"relays\":[";
  for (size_t i = 0; i < relays.size(); i++) {
    const RelayConfig& cfg = relays.config(i);
    if (i) json += ',';
    json += "{\"param\":\"" + jsonEscape(cfg.param) + "\",\"pin\":" + String(cfg.pin) +
            ",\"mode\":\"" + modeName(cfg.mode) + "\",\"input\":" + (relays.inputActive(i) ? "true" : "false") +
            ",\"on\":" + (relays.isOn(i) ? "true" : "false") + "}";
  }

  json += "],\"params\":[";
  bool first = true;
  for (const auto& kv : seenParams) {
    if (!first) json += ',';
    first = false;
    json += "{\"name\":\"" + jsonEscape(kv.first) + "\",\"value\":\"" + jsonEscape(kv.second.value) +
            "\",\"ageMs\":" + String(now - kv.second.updatedAt) + "}";
  }
  json += "]}";
  web.send(200, "application/json", json);
}

void handleRelay() {
  if (!ENABLE_WEB_CONTROL) return web.send(403, "text/plain", "web control disabled");
  if (!web.hasArg("i") || !web.hasArg("on")) return web.send(400, "text/plain", "need i and on");
  long index = web.arg("i").toInt();
  if (index < 0 || static_cast<size_t>(index) >= relays.size()) return web.send(404, "text/plain", "no such relay");
  relays.set(index, web.arg("on") == "1");
  web.send(204);
}

void startWeb() {
  web.on("/", HTTP_GET, [] { web.send_P(200, "text/html", kIndexHtml); });
  web.on("/api/state", HTTP_GET, handleState);
  web.on("/api/relay", HTTP_POST, handleRelay);
  web.begin();
}

// ----- WiFi --------------------------------------------------------------------------------------

void onWifiUp() {
  udp.stop();
  udp.begin(OSC_LISTEN_PORT);
  MDNS.end();
  if (!webStarted) {
    startWeb();
    if (ENABLE_OSCQUERY) oscquery::begin(OSCQUERY_NAME, OSCQUERY_HTTP_PORT, OSC_LISTEN_PORT);
    webStarted = true;
  }
  if (MDNS.begin(DEVICE_HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    if (ENABLE_OSCQUERY) oscquery::advertise();
  } else {
    Serial.println("[wifi] mDNS failed to start - OSCQuery discovery won't work");
  }

  String ip = WiFi.localIP().toString();
  Serial.printf("[wifi] connected, IP %s (RSSI %d dBm)\n", ip.c_str(), WiFi.RSSI());
  Serial.printf("[wifi] status page: http://%s.local/  or  http://%s/\n", DEVICE_HOSTNAME, ip.c_str());
  if (ENABLE_OSCQUERY) {
    Serial.println("[wifi] VRChat should find this board by itself (HUD: \"sending data to ...\")");
    Serial.printf("[wifi] fallback VRChat launch option: --osc=9000:%s:%u\n", ip.c_str(), OSC_LISTEN_PORT);
  } else {
    Serial.printf("[wifi] VRChat launch option: --osc=9000:%s:%u\n", ip.c_str(), OSC_LISTEN_PORT);
  }
}

void checkWifi() {
  bool up = WiFi.status() == WL_CONNECTED;
  if (up == wifiUp) return;
  wifiUp = up;
  if (up) {
    onWifiUp();
  } else {
    // We can no longer hear "contact released", so don't leave anything switched on.
    Serial.println("[wifi] disconnected - all relays off, reconnecting...");
    relays.allOff();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== VRChat OSC relay bridge ===");

  relays.begin(onRelayChanged);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.setSleep(false);  // modem sleep adds 100ms+ of latency to incoming UDP
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[wifi] connecting to \"%s\"...\n", WIFI_SSID);
}

void loop() {
  checkWifi();
  if (wifiUp) {
    pollOsc();
    web.handleClient();
    oscquery::handleClient();
  }
  relays.update();
  delay(1);
}
