#include "bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "hal.h"
#include "osc.h"

namespace bridge {
namespace {

constexpr char kParamPrefix[] = "/avatar/parameters/";
constexpr size_t kParamPrefixLen = sizeof(kParamPrefix) - 1;
constexpr size_t kMaxTrackedParams = 128;

struct SeenParam {
  std::string value;
  uint32_t updatedAt;
};

RelayBank relays(RELAYS, sizeof(RELAYS) / sizeof(RELAYS[0]));
SendFn sendRaw = nullptr;
DeviceInfo device;

std::string vrchatIp;
uint32_t lastPacketAt = 0;
uint32_t packetCount = 0;
uint32_t malformedCount = 0;
std::string currentAvatar;
std::map<std::string, SeenParam> seenParams;  // everything VRChat reported, for the status page
std::string oscqueryClient;
uint32_t oscqueryAt = 0;
uint8_t txBuf[256];

const char kJson[] = "application/json";

std::string formatValue(const osc::Value& v) {
  char buf[24];
  switch (v.type) {
    case osc::Value::Type::Bool: return v.b ? "true" : "false";
    case osc::Value::Type::Int: snprintf(buf, sizeof buf, "%ld", static_cast<long>(v.i)); return buf;
    case osc::Value::Type::Float: snprintf(buf, sizeof buf, "%.3f", v.f); return buf;
    case osc::Value::Type::String: return v.s;
    default: return "-";
  }
}

std::string jsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 2);
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

std::string str(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }
std::string num(unsigned long v) { return std::to_string(v); }
const char* boolean(bool b) { return b ? "true" : "false"; }

// ----- OSC ---------------------------------------------------------------------------------------

void sendToVrchat(const char* address, const osc::Value& value) {
  if (!sendRaw || vrchatIp.empty()) return;
  size_t n = osc::buildMessage(txBuf, sizeof txBuf, address, value);
  if (n > 0) sendRaw(txBuf, n);
}

void onRelayChanged(size_t index, bool on) {
  const RelayConfig& cfg = relays.config(index);
  hal::log("[relay] %-20s GPIO%-2u -> %s\n", cfg.param, cfg.pin, on ? "ON" : "off");
  if (cfg.feedbackParam) {
    std::string address = std::string(kParamPrefix) + cfg.feedbackParam;
    sendToVrchat(address.c_str(), osc::Value::fromBool(on));
  }
}

void onOscMessage(const char* address, const osc::Value& value) {
  if (strncmp(address, kParamPrefix, kParamPrefixLen) == 0) {
    const char* name = address + kParamPrefixLen;
    auto it = seenParams.find(name);
    if (it != seenParams.end()) {
      it->second = {formatValue(value), hal::millis()};
    } else if (seenParams.size() < kMaxTrackedParams) {
      seenParams[name] = {formatValue(value), hal::millis()};
      hal::log("[osc] new parameter: %s = %s\n", name, formatValue(value).c_str());
    }

    bool mapped = relays.handleParameter(name, value.asFloat());
    if (LOG_ALL_PARAMS || mapped) hal::log("[osc] %s = %s\n", name, formatValue(value).c_str());
    return;
  }

  if (strcmp(address, "/avatar/change") == 0) {
    currentAvatar = value.type == osc::Value::Type::String ? value.s : "";
    hal::log("[osc] avatar changed: %s - all relays off\n", currentAvatar.c_str());
    relays.allOff();
    seenParams.clear();
    return;
  }

  if (LOG_ALL_PARAMS) hal::log("[osc] %s = %s\n", address, formatValue(value).c_str());
}

// ----- Web status page ---------------------------------------------------------------------------

const char kIndexHtml[] = R"HTML(<!doctype html>
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
    `${esc(s.device)} ${esc(s.ip)} &middot; ${s.oscquery?'OSCQuery on, fallback':'VRChat'} launch option: <code>--osc=9000:${esc(s.ip)}:${s.port}</code><br>`+
    (s.oscquery?(s.oscqueryClient?`OSCQuery: discovered by ${esc(s.oscqueryClient)} ${(s.oscqueryAgeMs/1000).toFixed(0)} s ago<br>`:`OSCQuery: advertised as ${esc(s.oscqueryName)}, not discovered yet<br>`):'')+
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

std::string stateJson() {
  uint32_t now = hal::millis();
  std::string json;
  json.reserve(512 + seenParams.size() * 64);
  json += "{\"device\":" + str(device.kind);
  json += ",\"ip\":" + str(device.ip);
  json += ",\"port\":" + num(device.oscPort);
  json += ",\"vrchat\":" + (vrchatIp.empty() ? std::string("null") : str(vrchatIp));
  json += ",\"lastPacketMs\":" + num(now - lastPacketAt);
  json += ",\"packets\":" + num(packetCount);
  json += ",\"malformed\":" + num(malformedCount);
  json += ",\"avatar\":" + str(currentAvatar);
  json += ",\"webControl\":" + std::string(boolean(ENABLE_WEB_CONTROL));
  json += ",\"oscquery\":" + std::string(boolean(!device.oscqueryName.empty()));
  json += ",\"oscqueryName\":" + str(device.oscqueryName);
  json += ",\"oscqueryClient\":" + str(oscqueryClient);
  json += ",\"oscqueryAgeMs\":" + num(now - oscqueryAt);

  json += ",\"relays\":[";
  for (size_t i = 0; i < relays.size(); i++) {
    const RelayConfig& cfg = relays.config(i);
    if (i) json += ',';
    json += "{\"param\":" + str(cfg.param) + ",\"pin\":" + num(cfg.pin) + ",\"mode\":\"" + modeName(cfg.mode) +
            "\",\"input\":" + boolean(relays.inputActive(i)) + ",\"on\":" + boolean(relays.isOn(i)) + "}";
  }

  json += "],\"params\":[";
  bool first = true;
  for (const auto& kv : seenParams) {
    if (!first) json += ',';
    first = false;
    json += "{\"name\":" + str(kv.first) + ",\"value\":" + str(kv.second.value) +
            ",\"ageMs\":" + num(now - kv.second.updatedAt) + "}";
  }
  json += "]}";
  return json;
}

// ----- OSCQuery ----------------------------------------------------------------------------------
// ACCESS: 0 = none, 1 = read, 2 = write, 3 = read/write. We only receive, so /avatar is write-only.

const char kChangeNode[] = R"({"FULL_PATH":"/avatar/change","ACCESS":2,"TYPE":"s"})";
const char kAvatarNode[] =
    R"({"FULL_PATH":"/avatar","ACCESS":2,"CONTENTS":{"change":{"FULL_PATH":"/avatar/change","ACCESS":2,"TYPE":"s"}}})";
const char kRootNode[] =
    R"({"DESCRIPTION":"root node","FULL_PATH":"/","ACCESS":0,"CONTENTS":{"avatar":)"
    R"({"FULL_PATH":"/avatar","ACCESS":2,"CONTENTS":{"change":{"FULL_PATH":"/avatar/change","ACCESS":2,"TYPE":"s"}}}}})";

std::string hostInfoJson() {
  return "{\"NAME\":" + str(device.oscqueryName) + ",\"OSC_IP\":" + str(device.ip) +
         ",\"OSC_PORT\":" + num(device.oscPort) +
         ",\"OSC_TRANSPORT\":\"UDP\",\"EXTENSIONS\":{\"ACCESS\":true,\"CLIPMODE\":false,"
         "\"RANGE\":true,\"TYPE\":true,\"VALUE\":true}}";
}

}  // namespace

void begin(SendFn sendToVrchatFn) {
  sendRaw = sendToVrchatFn;
  relays.begin(onRelayChanged);
}

void setDeviceInfo(const DeviceInfo& info) { device = info; }

void handlePacket(const uint8_t* data, size_t len, const std::string& fromIp) {
  vrchatIp = fromIp;
  lastPacketAt = hal::millis();
  packetCount++;
  if (len == 0 || !osc::parsePacket(data, len, onOscMessage)) malformedCount++;
}

void update() { relays.update(); }

void allOff(const char* reason) {
  hal::log("[bridge] %s - all relays off\n", reason);
  relays.allOff();
}

HttpResponse handleWeb(const HttpRequest& req) {
  if (req.path == "/" && req.method == "GET") return HttpResponse(200, "text/html", kIndexHtml);
  if (req.path == "/api/state" && req.method == "GET") return HttpResponse(200, kJson, stateJson());
  if (req.path == "/api/relay" && req.method == "POST") {
    if (!ENABLE_WEB_CONTROL) return HttpResponse(403, "text/plain", "web control disabled");
    auto i = req.args.find("i");
    auto on = req.args.find("on");
    if (i == req.args.end() || on == req.args.end()) return HttpResponse(400, "text/plain", "need i and on");
    long index = strtol(i->second.c_str(), nullptr, 10);
    if (index < 0 || static_cast<size_t>(index) >= relays.size()) {
      return HttpResponse(404, "text/plain", "no such relay");
    }
    relays.set(index, on->second == "1");
    return HttpResponse(204, "text/plain", "");
  }
  return HttpResponse(404, "text/plain", "not found");
}

// OSCQuery serves the node at the requested path; HOST_INFO is a query flag on any path.
HttpResponse handleOscQuery(const HttpRequest& req) {
  if (req.args.count("HOST_INFO")) {
    if (req.clientIp != oscqueryClient) hal::log("[oscquery] HOST_INFO read by %s\n", req.clientIp.c_str());
    oscqueryClient = req.clientIp;
    oscqueryAt = hal::millis();
    return HttpResponse(200, kJson, hostInfoJson());
  }

  std::string path = req.path;
  if (path.size() > 1 && path.back() == '/') path.pop_back();
  if (path == "/") return HttpResponse(200, kJson, kRootNode);
  if (path == "/avatar") return HttpResponse(200, kJson, kAvatarNode);
  if (path == "/avatar/change") return HttpResponse(200, kJson, kChangeNode);
  return HttpResponse(404, kJson, "{}");
}

}  // namespace bridge
