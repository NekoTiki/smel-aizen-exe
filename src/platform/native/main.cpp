// VRChat OSC -> relay bridge, PC twin (Windows).
//
// Runs the same shared core as the ESP32 firmware (src/core) on your PC, so you can develop and
// test with real VRChat before having hardware. Differences from the board:
//   - WiFi is the PC's network stack. "wifi down" / "wifi up" on the console simulate a drop.
//   - GPIO pins are simulated and their levels are printed instead of driving relays.
//   - mDNS goes through Windows' own responder (DnsServiceRegister), so VRChat discovers the twin
//     through OSCQuery exactly as it would discover the ESP32.
//
// Build: pio run -e twin     Run: .pio\build\twin\program.exe [--help]

#ifndef _WIN32
#error "The PC twin currently supports Windows only"
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
#include <mstcpip.h>

#ifndef SIO_UDP_CONNRESET  // missing from some MinGW headers
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bridge.h"
#include "config.h"
#include "hal.h"

// ----- Simulated hardware -------------------------------------------------------------------------

namespace {
const auto kBootTime = std::chrono::steady_clock::now();
int pinLevel[64];  // -1 = never written
bool pinIsOutput[64];
}  // namespace

namespace hal {

uint32_t millis() {
  auto elapsed = std::chrono::steady_clock::now() - kBootTime;
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void pinOutput(uint8_t pin) {
  if (pin < 64) pinIsOutput[pin] = true;
}

void pinWrite(uint8_t pin, bool high) {
  if (pin >= 64 || pinLevel[pin] == static_cast<int>(high)) return;
  pinLevel[pin] = high;
  // Before pinMode(OUTPUT) the real board only latches the level, so only report real outputs.
  if (pinIsOutput[pin]) log("[gpio]  GPIO%-2u = %s\n", pin, high ? "HIGH" : "LOW");
}

void log(const char* fmt, ...) {
  char stamp[16];
  uint32_t ms = millis();
  snprintf(stamp, sizeof stamp, "%5u.%03u ", ms / 1000, ms % 1000);
  fputs(stamp, stdout);
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
}

}  // namespace hal

namespace {

struct Options {
  uint16_t oscPort = 9101;  // not 9001: that's where VRChat already sends by default
  uint16_t webPort = 8000;
  uint16_t oscqueryPort = OSCQUERY_HTTP_PORT;
  bool oscquery = ENABLE_OSCQUERY;
  // Listen on the network and advertise the LAN IP, like the ESP32. VRChat doesn't pick up
  // OSCQuery services announced at 127.0.0.1, so --local (loopback only) needs the launch option.
  bool lan = true;
  std::string name = std::string(OSCQUERY_NAME) + "-TWIN";
};

Options opts;
std::atomic<bool> running{true};
bool wifiUp = true;

SOCKET udpSock = INVALID_SOCKET;
SOCKET webSock = INVALID_SOCKET;
SOCKET oscquerySock = INVALID_SOCKET;

// Open /api/events streams (live status page). Capped like on the ESP32, oldest dropped first.
constexpr size_t kMaxEventClients = 3;
std::vector<SOCKET> eventClients;
sockaddr_in vrchatAddr = {};

// ----- Networking --------------------------------------------------------------------------------

std::string ipToString(const in_addr& addr) {
  char buf[INET_ADDRSTRLEN] = "?";
  inet_ntop(AF_INET, &addr, buf, sizeof buf);
  return buf;
}

// The LAN address other machines would use. No packet is sent: connect() on UDP only picks a route.
std::string lanIp() {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in probe = {};
  probe.sin_family = AF_INET;
  probe.sin_port = htons(53);
  inet_pton(AF_INET, "192.0.2.1", &probe.sin_addr);
  std::string ip = "127.0.0.1";
  if (connect(s, reinterpret_cast<sockaddr*>(&probe), sizeof probe) == 0) {
    sockaddr_in local = {};
    int len = sizeof local;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0) ip = ipToString(local.sin_addr);
  }
  closesocket(s);
  return ip;
}

SOCKET bindSocket(int type, uint16_t port, const char* what) {
  SOCKET s = socket(AF_INET, type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(opts.lan ? INADDR_ANY : INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || (type == SOCK_STREAM && listen(s, 8) != 0)) {
    fprintf(stderr, "error: can't open %s port %u (in use?) - pick another with the options in --help\n", what,
            port);
    exit(1);
  }
  return s;
}

void sendToVrchat(const uint8_t* data, size_t len) {
  sockaddr_in to = vrchatAddr;
  to.sin_port = htons(VRCHAT_IN_PORT);
  sendto(udpSock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&to),
         sizeof to);
}

void pollOsc() {
  static uint8_t rxBuf[1536];
  for (;;) {
    sockaddr_in from = {};
    int fromLen = sizeof from;
    int n = recvfrom(udpSock, reinterpret_cast<char*>(rxBuf), sizeof rxBuf, 0, reinterpret_cast<sockaddr*>(&from),
                     &fromLen);
    if (n < 0) {
      int err = WSAGetLastError();
      if (err != WSAEMSGSIZE) return;  // WSAEWOULDBLOCK: nothing left to read
      n = 0;                           // oversized packet, count it as malformed like the ESP32 does
    }
    if (!wifiUp) continue;  // "WiFi" is down: the packet never reaches the board
    vrchatAddr = from;
    bridge::handlePacket(rxBuf, n, ipToString(from.sin_addr));
  }
}

std::string urlDecode(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    if (in[i] == '+') {
      out += ' ';
    } else if (in[i] == '%' && i + 2 < in.size()) {
      out += static_cast<char>(strtol(in.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out += in[i];
    }
  }
  return out;
}

const char* reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    default: return "Error";
  }
}

// ----- Live status page (Server-Sent Events) -----------------------------------------------------

bool sendAll(SOCKET s, const std::string& data) {
  return send(s, data.data(), static_cast<int>(data.size()), 0) == static_cast<int>(data.size());
}

void dropEventClient(size_t index, const char* why) {
  closesocket(eventClients[index]);
  eventClients.erase(eventClients.begin() + index);
  hal::log("[web] live page %s (%u open)\n", why, static_cast<unsigned>(eventClients.size()));
}

void dropAllEventClients() {
  while (!eventClients.empty()) dropEventClient(0, "disconnected");
}

void startEventStream(SOCKET c) {
  // A stalled browser must not freeze the loop: give up on a send after 200 ms and drop it.
  DWORD sendTimeoutMs = 200;
  setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeoutMs), sizeof sendTimeoutMs);
  if (!sendAll(c, std::string(bridge::kEventStreamHeaders) + bridge::eventSnapshot())) {
    closesocket(c);
    return;
  }
  if (eventClients.size() >= kMaxEventClients) dropEventClient(0, "dropped (too many open)");
  eventClients.push_back(c);
  hal::log("[web] live page connected (%u open)\n", static_cast<unsigned>(eventClients.size()));
}

void broadcastEvents() {
  std::string events = bridge::takeEvents(!eventClients.empty());
  if (events.empty()) return;
  for (size_t i = eventClients.size(); i-- > 0;) {
    if (!sendAll(eventClients[i], events)) dropEventClient(i, "disconnected");
  }
}

// Browsers never send anything on an event stream, so readable means closed (recv returns 0).
void checkEventClients(const fd_set& readable) {
  for (size_t i = eventClients.size(); i-- > 0;) {
    if (!FD_ISSET(eventClients[i], &readable)) continue;
    char buf[256];
    if (recv(eventClients[i], buf, sizeof buf, 0) <= 0) dropEventClient(i, "disconnected");
  }
}

// A deliberately small HTTP/1.1 server: one request per connection, like the ESP32 WebServer.
void serveHttp(SOCKET listener, bridge::HttpResponse (*handler)(const bridge::HttpRequest&)) {
  sockaddr_in peer = {};
  int peerLen = sizeof peer;
  SOCKET c = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLen);
  if (c == INVALID_SOCKET) return;
  if (!wifiUp) {
    closesocket(c);
    return;
  }
  DWORD timeoutMs = 1000;
  setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof timeoutMs);

  std::string raw;
  char buf[2048];
  while (raw.find("\r\n\r\n") == std::string::npos && raw.size() < 16384) {
    int n = recv(c, buf, sizeof buf, 0);
    if (n <= 0) break;
    raw.append(buf, n);
  }

  size_t sp1 = raw.find(' ');
  size_t sp2 = sp1 == std::string::npos ? std::string::npos : raw.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) {
    closesocket(c);
    return;
  }

  bridge::HttpRequest req;
  req.method = raw.substr(0, sp1);
  std::string target = raw.substr(sp1 + 1, sp2 - sp1 - 1);
  size_t q = target.find('?');
  req.path = urlDecode(target.substr(0, q));
  if (q != std::string::npos) {
    std::string query = target.substr(q + 1);
    size_t start = 0;
    while (start <= query.size()) {
      size_t amp = query.find('&', start);
      std::string pair = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
      if (!pair.empty()) {
        size_t eq = pair.find('=');
        req.args[urlDecode(pair.substr(0, eq))] = eq == std::string::npos ? "" : urlDecode(pair.substr(eq + 1));
      }
      if (amp == std::string::npos) break;
      start = amp + 1;
    }
  }
  req.clientIp = ipToString(peer.sin_addr);

  if (listener == webSock && req.method == "GET" && req.path == bridge::kEventsPath) {
    startEventStream(c);  // the socket stays open
    return;
  }

  bridge::HttpResponse res = handler(req);
  std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) +
                    "\r\nContent-Type: " + res.contentType + "\r\nContent-Length: " +
                    std::to_string(res.body.size()) + "\r\nConnection: close\r\n\r\n" + res.body;
  send(c, out.data(), static_cast<int>(out.size()), 0);
  shutdown(c, SD_SEND);
  closesocket(c);
}

// ----- mDNS via Windows' responder ---------------------------------------------------------------

struct MdnsService {
  std::wstring instanceName;  // "<name>._oscjson._tcp.local"
  PDNS_SERVICE_INSTANCE instance = nullptr;
  DNS_SERVICE_REGISTER_REQUEST request = {};
  std::string label;
};

MdnsService mdnsServices[2];

VOID WINAPI onMdnsRegistered(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
  auto* svc = static_cast<MdnsService*>(context);
  if (status == ERROR_SUCCESS) {
    hal::log("[mdns] %s: registered with Windows, now visible on the network\n", svc->label.c_str());
  } else {
    hal::log("[mdns] %s: registration FAILED (error %lu) - VRChat won't find the twin\n", svc->label.c_str(),
             status);
  }
  if (instance) DnsServiceFreeInstance(instance);
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

// The services point at the PC's own mDNS name; the twin doesn't claim DEVICE_HOSTNAME.local.
std::string pcHostName() {
  char host[256];
  DWORD hostLen = sizeof host;
  if (!GetComputerNameExA(ComputerNameDnsHostname, host, &hostLen)) return "localhost.local";
  return std::string(host) + ".local";
}

void registerMdns(MdnsService& svc, const std::string& type, uint16_t port, const std::string& ip,
                  const char* what) {
  std::string host = pcHostName();
  svc.label = type;
  svc.instanceName = widen(opts.name + "." + type + ".local");
  hal::log("[mdns] service   %s  \"%s\" port %u -> %s (%s)  (%s)\n", type.c_str(), opts.name.c_str(), port,
           host.c_str(), ip.c_str(), what);

  std::wstring hostName = widen(host);
  IP4_ADDRESS addr = 0;
  inet_pton(AF_INET, ip.c_str(), &addr);
  svc.instance = DnsServiceConstructInstance(svc.instanceName.c_str(), hostName.c_str(), &addr, nullptr, port, 0, 0,
                                             0, nullptr, nullptr);
  svc.request.Version = DNS_QUERY_REQUEST_VERSION1;
  svc.request.InterfaceIndex = 0;
  svc.request.pServiceInstance = svc.instance;
  svc.request.pRegisterCompletionCallback = onMdnsRegistered;
  svc.request.pQueryContext = &svc;
  svc.request.unicastEnabled = FALSE;
  DWORD result = DnsServiceRegister(&svc.request, nullptr);
  if (result != DNS_REQUEST_PENDING) {
    hal::log("[mdns] %s: registration FAILED (error %lu, needs Windows 10 1809+)\n", svc.label.c_str(), result);
  }
}

void deregisterMdns() {
  for (MdnsService& svc : mdnsServices) {
    if (!svc.instance) continue;
    svc.request.pRegisterCompletionCallback = nullptr;
    DnsServiceDeRegister(&svc.request, nullptr);
    DnsServiceFreeInstance(svc.instance);
    svc.instance = nullptr;
    hal::log("[mdns] %s: removed\n", svc.label.c_str());
  }
}

// ----- Console -----------------------------------------------------------------------------------

std::mutex commandsMutex;
std::deque<std::string> commands;

void readConsole() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::lock_guard<std::mutex> lock(commandsMutex);
    commands.push_back(line);
  }
}

void printCommands() {
  printf("commands: wifi down | wifi up | help | quit\n");
  fflush(stdout);
}

void runCommands() {
  std::deque<std::string> pending;
  {
    std::lock_guard<std::mutex> lock(commandsMutex);
    pending.swap(commands);
  }
  for (const std::string& cmd : pending) {
    if (cmd == "wifi down" && wifiUp) {
      wifiUp = false;
      bridge::allOff("WiFi disconnected, reconnecting");
      dropAllEventClients();  // like the board: open pages lose their connection and retry
    } else if (cmd == "wifi up" && !wifiUp) {
      wifiUp = true;
      hal::log("[wifi] connected again\n");
    } else if (cmd == "quit" || cmd == "exit") {
      running = false;
    } else if (!cmd.empty()) {
      printCommands();
    }
  }
}

BOOL WINAPI onConsoleCtrl(DWORD) {
  running = false;
  return TRUE;
}

void usage() {
  printf(
      "VRChat OSC relay bridge - PC twin\n\n"
      "  --osc-port N        UDP port for OSC from VRChat (default %u)\n"
      "  --web-port N        status page port (default %u)\n"
      "  --oscquery-port N   OSCQuery HTTP port (default %u)\n"
      "  --name NAME         OSCQuery service name (default %s)\n"
      "  --no-oscquery       don't advertise; point VRChat at the twin with the launch option\n"
      "  --local             loopback only: nothing reachable from the network, no OSCQuery discovery;\n"
      "                      VRChat needs the launch option --osc=9000:127.0.0.1:<osc-port>\n"
      "\n"
      "By default the twin listens on the network and advertises its LAN IP, like the ESP32.\n"
      "Windows Firewall asks once; allowing private networks is enough.\n",
      opts.oscPort, opts.webPort, opts.oscqueryPort, opts.name.c_str());
}

void parseArgs(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto value = [&]() -> const char* {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s needs a value\n", a.c_str());
        exit(2);
      }
      return argv[++i];
    };
    if (a == "--osc-port") opts.oscPort = static_cast<uint16_t>(atoi(value()));
    else if (a == "--web-port") opts.webPort = static_cast<uint16_t>(atoi(value()));
    else if (a == "--oscquery-port") opts.oscqueryPort = static_cast<uint16_t>(atoi(value()));
    else if (a == "--name") opts.name = value();
    else if (a == "--no-oscquery") opts.oscquery = false;
    else if (a == "--local") opts.lan = false;
    else if (a == "--lan") opts.lan = true;  // the default; kept for older scripts
    else {
      usage();
      exit(a == "--help" || a == "-h" ? 0 : 2);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  for (int& level : pinLevel) level = -1;
  parseArgs(argc, argv);
  SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fprintf(stderr, "error: WSAStartup failed\n");
    return 1;
  }

  printf("\n=== VRChat OSC relay bridge === (PC twin, pins as on %s)\n",
#ifdef TWIN_BOARD_ESP32S3
         "ESP32-S3-DevKitC-1"
#else
         "generic ESP32"
#endif
  );

  bridge::begin(sendToVrchat);

  udpSock = bindSocket(SOCK_DGRAM, opts.oscPort, "OSC");
  u_long nonBlocking = 1;
  ioctlsocket(udpSock, FIONBIO, &nonBlocking);
  // Windows reports an earlier ICMP "port unreachable" as a recv error on UDP; ignore those.
  BOOL noReset = FALSE;
  DWORD bytes = 0;
  WSAIoctl(udpSock, SIO_UDP_CONNRESET, &noReset, sizeof noReset, nullptr, 0, &bytes, nullptr, nullptr);

  webSock = bindSocket(SOCK_STREAM, opts.webPort, "status page");
  if (opts.oscquery) oscquerySock = bindSocket(SOCK_STREAM, opts.oscqueryPort, "OSCQuery");

  std::string ip = opts.lan ? lanIp() : "127.0.0.1";
  bool discoverable = opts.oscquery && opts.lan;
  bridge::setDeviceInfo({"PC twin", ip, opts.oscPort, discoverable ? opts.name : ""});

  if (opts.lan) {
    hal::log("[wifi] connected, IP %s (listening on the network, like the ESP32)\n", ip.c_str());
  } else {
    hal::log("[wifi] connected, IP %s (--local: loopback only, unreachable from the network)\n", ip.c_str());
  }

  // mDNS: what the twin announces. Unlike the ESP32 it claims no hostname and no _http._tcp.
  hal::log("[mdns] hostname  none - the twin doesn't register %s.local (only the ESP32 does)\n", DEVICE_HOSTNAME);
  if (discoverable) {
    registerMdns(mdnsServices[0], "_oscjson._tcp", opts.oscqueryPort, ip, "OSCQuery, VRChat reads this");
    registerMdns(mdnsServices[1], "_osc._udp", opts.oscPort, ip, "OSC input");
  } else if (!opts.oscquery) {
    hal::log("[mdns] OSCQuery off (--no-oscquery): nothing announced, VRChat won't find the twin by itself\n");
  } else {
    hal::log("[mdns] nothing announced: VRChat ignores OSCQuery services at 127.0.0.1 (drop --local)\n");
  }

  if (opts.lan) {
    hal::log("[web] status page  http://localhost:%u/  or  http://%s:%u/\n", opts.webPort, ip.c_str(), opts.webPort);
  } else {
    hal::log("[web] status page  http://localhost:%u/\n", opts.webPort);
  }
  hal::log("[osc] listening on UDP %u (the ESP32 uses %u)\n", opts.oscPort, OSC_LISTEN_PORT);
  if (discoverable) {
    hal::log("[vrchat] should find the twin by itself (HUD: \"sending data to %s\")\n", opts.name.c_str());
    hal::log("[vrchat] fallback launch option: --osc=9000:%s:%u\n", ip.c_str(), opts.oscPort);
  } else {
    hal::log("[vrchat] launch option needed: --osc=9000:%s:%u\n", ip.c_str(), opts.oscPort);
  }
  printCommands();

  std::thread(readConsole).detach();

  while (running) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(udpSock, &readable);
    FD_SET(webSock, &readable);
    if (oscquerySock != INVALID_SOCKET) FD_SET(oscquerySock, &readable);
    for (SOCKET s : eventClients) FD_SET(s, &readable);
    timeval tick = {0, 5000};  // 5 ms, about how often the ESP32 loop comes around
    if (select(0, &readable, nullptr, nullptr, &tick) > 0) {
      checkEventClients(readable);  // before serveHttp can change the list
      if (FD_ISSET(udpSock, &readable)) pollOsc();
      if (FD_ISSET(webSock, &readable)) serveHttp(webSock, bridge::handleWeb);
      if (oscquerySock != INVALID_SOCKET && FD_ISSET(oscquerySock, &readable)) {
        serveHttp(oscquerySock, bridge::handleOscQuery);
      }
    }
    runCommands();
    bridge::update();
    broadcastEvents();
  }

  hal::log("[twin] shutting down\n");
  bridge::allOff("shutdown");
  dropAllEventClients();
  deregisterMdns();
  WSACleanup();
  return 0;
}
