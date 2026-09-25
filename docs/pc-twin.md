# PC twin: run the project without an ESP32

The PC twin is the firmware's shared core (`src/core`: OSC parsing, relay modes, safety rules,
status page, OSCQuery) compiled as a Windows program. Only the thin hardware layer is swapped:

| | ESP32 firmware | PC twin |
|---|---|---|
| Relay logic, OSC, status page, OSCQuery | `src/core` | **same code** |
| Network | ESP32 WiFi | the PC's network |
| mDNS / OSCQuery discovery | ESPmDNS | Windows' built-in mDNS responder |
| Relays | GPIO pins | simulated: pin levels printed to the console |
| OSC port | 9001 | 9101 (9001 is VRChat's default output, other apps use it) |
| Status page | `http://vrc-relay.local/` (port 80) | `http://localhost:8000/` |
| WiFi drop | real | type `wifi down` / `wifi up` in the console |

VRChat discovers the twin through OSCQuery exactly as it discovers the board, and shows
*"sending data to VRC-Relay-ESP32-TWIN"*. So you can test your avatar's contacts with real VRChat
before any hardware exists. Pins follow the ESP32-S3-DevKitC-1 layout (GPIO 4–7).

## Setup

1. Install a MinGW-w64 GCC (one time, no admin needed):
   ```
   winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
   ```
2. Build and run it from VS Code, or with `pio run -e twin -t run_twin` on the command line.

The program is statically linked, so you can copy `.pio\build\twin\program.exe` anywhere, for
example to your friend's PC.

### In VS Code (PlatformIO extension)

- **Run:** PlatformIO sidebar (alien icon) → *Project Tasks* → **twin** → *Custom* →
  **Run PC twin**, or **Run PC twin (LAN)** for `--lan`. It builds first if needed, then runs in
  the terminal panel. Type `wifi down`, `wifi up` or `quit` there. Ctrl+C also stops it.
- **Build only:** *twin* → *General* → **Build**.
- **Code highlighting:** IntelliSense follows the *active* environment. Pick `env:twin` in the
  status bar environment switcher (or *PlatformIO: Switch Project Environment*) while editing
  `src/platform/native/main.cpp`. Switch back to `env:esp32-s3-devkitc-1` for the firmware side.
  With the wrong environment selected you'll see false red squiggles, but builds are unaffected.
- You don't need `g++` on PATH. If it isn't there (common right after installing, until you sign
  out and back in), the build looks in the usual WinLibs/MSYS2/MinGW install folders and prints
  `Using g++ from ...`. If it finds nothing, it tells you how to install one.

## Using it with VRChat

1. Start the twin, then start VRChat with OSC enabled (Action Menu → Options → OSC).
   No launch options needed.
2. Watch for the HUD notice. The console prints `[oscquery] HOST_INFO read by 127.0.0.1` when
   VRChat has found the twin.
3. Touch a contact. The console shows the full chain the ESP32 would go through:
   ```
   4.885 [osc] RelayHeadpat = true
   4.885 [gpio]  GPIO4  = LOW
   4.885 [relay] RelayHeadpat         GPIO4  -> ON
   ```
   `LOW` = relay on, because the default relay boards are active-low.
4. `http://localhost:8000/` shows every parameter your avatar sends, which is the easiest way to get
   the exact names for `config.h`.

Rebuild after changing `include/config.h`, the same as you'd reflash the board.

## Without VRChat

```
python tools/fake_vrchat.py 127.0.0.1 set RelayHeadpat true --port 9101
python tools/fake_vrchat.py 127.0.0.1 tap RelayBoop --port 9101
python tools/fake_vrchat.py 127.0.0.1 sweep RelayProximity --port 9101
python tools/fake_vrchat.py 127.0.0.1 query                  # OSCQuery, as VRChat sees it
python tools/fake_vrchat.py listen 9000                      # feedback params (with VRChat closed)
```

## Options

```
program.exe --help
  --osc-port N        UDP port for OSC from VRChat (default 9101)
  --web-port N        status page port (default 8000)
  --oscquery-port N   OSCQuery HTTP port (default 8080)
  --name NAME         OSCQuery service name
  --no-oscquery       don't advertise; use the launch option --osc=9000:127.0.0.1:<osc-port>
  --lan               listen on the network and advertise the LAN IP, like the ESP32 does
```

By default the twin only listens on `127.0.0.1`, like other local OSCQuery apps. That's enough for
VRChat on the same PC, avoids a firewall prompt, and keeps the relay buttons off the network.
Use `--lan` to test from another device, for example VRChat on a Quest, or your friend's PC
talking to yours. Windows Firewall will ask for permission the first time.

## What the twin can't tell you

- WiFi range, latency and reconnect behaviour on the real board.
- Electrical issues: 3.3 V vs 5 V relay inputs, power, strapping pins (see the
  [board guide](esp32-s3-devkitc-1.md)).
- Whether your friend's router passes mDNS between the ESP32 and the PC. The twin runs on the same
  machine as VRChat, so it sidesteps guest networks and client isolation.

Everything else (which parameters arrive, how each relay mode reacts, avatar changes, the status
page, OSCQuery discovery) runs through the same code as on the ESP32.
