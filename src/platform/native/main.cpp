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
  bool lan = false;  // loopback only unless asked: the web controls have no authentication
  std::string name = std::string(OSCQUERY_NAME) + "-TWIN";
};

Options opts;
std::atomic<bool> running{true};
bool wifiUp = true;

SOCKET udpSock = INVALID_SOCKET;
SOCKET webSock = INVALID_SOCKET;
SOCKET oscquerySock = INVALID_SOCKET;
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
    hal::log("[oscquery] mDNS: %s registered\n", svc->label.c_str());
  } else {
    hal::log("[oscquery] mDNS: %s registration failed (error %lu)\n", svc->label.c_str(), status);
  }
  if (instance) DnsServiceFreeInstance(instance);
}

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void registerMdns(MdnsService& svc, const std::string& type, uint16_t port, const std::string& ip) {
  wchar_t host[256];
  DWORD hostLen = 256;
  if (!GetComputerNameExW(ComputerNameDnsHostname, host, &hostLen)) wcscpy(host, L"vrc-relay-twin");
  std::wstring hostName = std::wstring(host) + L".local";

  svc.label = opts.name + "." + type;
  svc.instanceName = widen(opts.name + "." + type + ".local");
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
    hal::log("[oscquery] mDNS: couldn't register %s (error %lu, needs Windows 10 1809+)\n", svc.label.c_str(),
             result);
  }
}

void deregisterMdns() {
  for (MdnsService& svc : mdnsServices) {
    if (!svc.instance) continue;
    svc.request.pRegisterCompletionCallback = nullptr;
    DnsServiceDeRegister(&svc.request, nullptr);
    DnsServiceFreeInstance(svc.instance);
    svc.instance = nullptr;
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
      "  --no-oscquery       don't advertise; point VRChat at the twin with --osc=9000:127.0.0.1:<osc-port>\n"
      "  --lan               listen on the network and advertise the LAN IP, like the ESP32 does\n"
      "                      (for VRChat on another PC or a Quest; Windows Firewall will ask)\n",
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
    else if (a == "--lan") opts.lan = true;
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

  // By default VRChat on this PC reaches the twin over loopback, as with any local OSCQuery app.
  std::string ip = opts.lan ? lanIp() : "127.0.0.1";
  bridge::setDeviceInfo({"PC twin", ip, opts.oscPort, opts.oscquery ? opts.name : ""});
  if (opts.oscquery) {
    registerMdns(mdnsServices[0], "_oscjson._tcp", opts.oscqueryPort, ip);
    registerMdns(mdnsServices[1], "_osc._udp", opts.oscPort, ip);
    hal::log("[oscquery] advertising \"%s\" (HTTP %u, OSC UDP %u)\n", opts.name.c_str(), opts.oscqueryPort,
             opts.oscPort);
  }

  hal::log("[wifi] connected, IP %s (%s)\n", ip.c_str(), opts.lan ? "PC network" : "loopback, --lan for network");
  hal::log("[wifi] status page: http://localhost:%u/\n", opts.webPort);
  if (opts.oscquery) {
    hal::log("[wifi] VRChat should find the twin by itself (HUD: \"sending data to %s\")\n", opts.name.c_str());
  }
  hal::log("[wifi] %sVRChat launch option: --osc=9000:%s:%u\n", opts.oscquery ? "fallback " : "", ip.c_str(),
           opts.oscPort);
  printCommands();

  std::thread(readConsole).detach();

  while (running) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(udpSock, &readable);
    FD_SET(webSock, &readable);
    if (oscquerySock != INVALID_SOCKET) FD_SET(oscquerySock, &readable);
    timeval tick = {0, 5000};  // 5 ms, about how often the ESP32 loop comes around
    if (select(0, &readable, nullptr, nullptr, &tick) > 0) {
      if (FD_ISSET(udpSock, &readable)) pollOsc();
      if (FD_ISSET(webSock, &readable)) serveHttp(webSock, bridge::handleWeb);
      if (oscquerySock != INVALID_SOCKET && FD_ISSET(oscquerySock, &readable)) {
        serveHttp(oscquerySock, bridge::handleOscQuery);
      }
    }
    runCommands();
    bridge::update();
  }

  hal::log("[twin] shutting down\n");
  bridge::allOff("shutdown");
  deregisterMdns();
  WSACleanup();
  return 0;
}
