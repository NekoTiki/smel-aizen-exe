#pragma once
// Everything you are expected to edit lives here.

#include "relays.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#warning "include/secrets.h not found - copy include/secrets.example.h to include/secrets.h"
#include "secrets.example.h"
#endif

// mDNS name: the status page is at http://vrc-relay.local/
#define DEVICE_HOSTNAME "vrc-relay"

// UDP port we receive OSC on. With OSCQuery VRChat learns it automatically; without it, use the
// launch option --osc=9000:<ESP32_IP>:9001.
constexpr uint16_t OSC_LISTEN_PORT = 9001;

// OSCQuery: VRChat discovers the board over mDNS and sends to it alongside any other OSC apps,
// with no launch option. The name (plus a MAC suffix) is what VRChat shows in its HUD.
constexpr bool ENABLE_OSCQUERY = true;
constexpr uint16_t OSCQUERY_HTTP_PORT = 8080;
#define OSCQUERY_NAME "VRC-Relay-ESP32"

// VRChat listens here. Only used for the optional feedback parameters below.
constexpr uint16_t VRCHAT_IN_PORT = 9000;

// Print every parameter update to serial. VRChat sends a lot of them (Velocity*, Angular*, ...),
// so this is off by default; new parameter names are always printed once when first seen.
constexpr bool LOG_ALL_PARAMS = false;

// Allow switching relays from the web status page. Anyone on your LAN can reach it.
constexpr bool ENABLE_WEB_CONTROL = true;

// ---------------------------------------------------------------------------------------------
// Relay mapping. `parameter` is the avatar parameter name driven by your Contact Receiver
// (the part after /avatar/parameters/). Several relays may listen to the same parameter.
//
// mode:
//   Follow - relay is closed while the contact is touched (Receiver type Constant, or a
//            Proximity float above `threshold`)
//   Pulse  - relay closes for `pulseMs` each time the contact is touched (Receiver OnEnter)
//   Toggle - each touch flips the relay
// activeLow: true for most cheap blue relay boards (IN pulled LOW = relay on).
// threshold: activation level for float parameters; bools are 0/1 so 0.5 works for them.
// maxOnMs:   safety cutoff, relay is forced open after this long (0 = no limit).
// feedback:  optional Bool avatar parameter the ESP32 sets to the relay state (nullptr = off).
//            It must exist in the avatar's Expression Parameters to have any effect.
//
// The pins below are picked per board at compile time (pio run -e <board>). The PC twin
// (pio run -e twin) uses the ESP32-S3-DevKitC-1 set.
// ---------------------------------------------------------------------------------------------
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || defined(TWIN_BOARD_ESP32S3)
// ESP32-S3-DevKitC-1: GPIO 4-7 are header J1 pins 4-7, with 5V and G at the bottom of the same
// header (pins 21, 22). Other free outputs: 1, 2, 8-18, 21, 47.
// Avoid: 0, 3, 45, 46 (strapping), 19/20 (USB), 43/44 (UART/serial monitor),
//        35-37 (used by octal PSRAM/flash on R8 and WROOM-2 variants), 38/48 (RGB LED).
// See docs/esp32-s3-devkitc-1.md.
constexpr uint8_t RELAY_PIN_1 = 4;
constexpr uint8_t RELAY_PIN_2 = 5;
constexpr uint8_t RELAY_PIN_3 = 6;
constexpr uint8_t RELAY_PIN_4 = 7;
#else
// Generic ESP32 devkit. Other free outputs: 4, 13, 16-19, 21-23, 32.
constexpr uint8_t RELAY_PIN_1 = 26;
constexpr uint8_t RELAY_PIN_2 = 27;
constexpr uint8_t RELAY_PIN_3 = 25;
constexpr uint8_t RELAY_PIN_4 = 33;
#endif

static const RelayConfig RELAYS[] = {
  // parameter          pin          mode               activeLow  threshold  pulseMs  maxOnMs  feedback
  { "RelayHeadpat",     RELAY_PIN_1, RelayMode::Follow, true,      0.5f,      0,       60000,   nullptr },
  { "RelayBoop",        RELAY_PIN_2, RelayMode::Pulse,  true,      0.5f,      500,     0,       nullptr },
  { "RelayHandToggle",  RELAY_PIN_3, RelayMode::Toggle, true,      0.5f,      0,       0,       "RelayHandToggle_State" },
  { "RelayProximity",   RELAY_PIN_4, RelayMode::Follow, true,      0.8f,      0,       60000,   nullptr },
};
