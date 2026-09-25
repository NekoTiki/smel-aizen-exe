# VRChat OSC → ESP32 relays

An ESP32 on your WiFi is discovered by VRChat through OSCQuery, receives its OSC output and switches relays when avatar
Contact Receivers are triggered. It also records every avatar parameter it hears and shows them on
a status page.

```
 VRChat (PC) ──UDP/OSC :9001──▶ ESP32 ──GPIO──▶ relay board
      ▲                           │
      └──── UDP/OSC :9000 ────────┘  (optional feedback parameters)
```

**No hardware yet?** The [PC twin](docs/pc-twin.md) runs the same code on Windows
(`pio run -e twin`). VRChat discovers it through OSCQuery like the real board, and relay switching
is shown in the console.

## 1. Firmware

Two boards are set up: **ESP32-S3-DevKitC-1** (the default, see
[docs/esp32-s3-devkitc-1.md](docs/esp32-s3-devkitc-1.md)) and a generic **ESP32 devkit**
(`pio run -e esp32dev -t upload`). The relay pins are picked per board in `config.h`.

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or `pip install platformio`).
2. `copy include\secrets.example.h include\secrets.h` and put your WiFi name/password in it.
   The ESP32 only supports **2.4 GHz** WiFi.
3. Edit the `RELAYS` table in [include/config.h](include/config.h): parameter name, GPIO, mode.
4. `pio run -t upload`, then `pio device monitor`. The monitor prints the ESP32's IP address and
   status page URL.

> Windows: if the build fails with `xtensa-esp32-elf-g++: error: CreateProcess: No such file or
> directory`, your PlatformIO folder path is too long. Set `PLATFORMIO_CORE_DIR` to a short path
> such as `C:\pio`.

## 2. VRChat avatar

For each thing that should switch a relay:

1. Add a **VRC Contact Receiver** to the avatar (for example on the head, collision tags `Hand`,
   `Finger`) and set its **Parameter**, for example `RelayHeadpat`.
   - Receiver type **Constant** → Bool, true while touched → use relay mode `Follow`
   - Receiver type **OnEnter** → Bool, true for a moment on touch → use `Pulse` or `Toggle`
   - Receiver type **Proximity** → Float 0..1 → `Follow` with a `threshold` such as `0.8`
2. **Add the same parameter to the avatar's Expression Parameters.** VRChat only sends parameters
   that are listed there over OSC. Untick *Synced* so it costs no sync bits.
3. Upload the avatar.

VRChat caches each avatar's OSC parameter list. After adding parameters, delete the avatar's file in
`%USERPROFILE%\AppData\LocalLow\VRChat\VRChat\OSC\<usr_id>\Avatars\`, or the new ones won't be sent.

## 3. Turn on OSC in VRChat

Action Menu → Options → OSC → **Enabled**. No launch options or IP addresses are needed.

The ESP32 advertises itself with OSCQuery, so VRChat finds it on its own. VRChat shows a HUD notice
like *"sending data to VRC-Relay-ESP32-A1B2C3"* and sends avatar parameters to it **in addition
to** its normal output, so your other OSC apps keep working.

Open `http://vrc-relay.local/` (or the IP from the serial monitor). It should show
*"OSCQuery: discovered by &lt;PC IP&gt;"*. Touch the contact and the relay should click.

### If VRChat doesn't find the ESP32

- Discovery uses mDNS (multicast), so the PC and ESP32 must be on the same network and subnet.
  Guest networks, "AP/client isolation" and some mesh routers block it.
- Windows Firewall must allow VRChat on private networks (mDNS replies arrive on UDP 5353).
- Restart VRChat with the ESP32 already online.
- `python tools/fake_vrchat.py <ESP32_IP> query` shows what the ESP32 offers to VRChat.
- Last resort: set `ENABLE_OSCQUERY = false` in `config.h` and add the Steam launch option
  `--osc=9000:<ESP32_IP>:9001`. Give the ESP32 a DHCP reservation so its IP stays the same.
  With this option VRChat sends OSC *only* to the ESP32, so other OSC apps stop receiving it.
  Don't combine it with OSCQuery, or the ESP32 gets every packet twice.

## Testing without VRChat

`tools/fake_vrchat.py` needs only the Python standard library:

```
python tools/fake_vrchat.py 192.168.1.50 set RelayHeadpat true
python tools/fake_vrchat.py 192.168.1.50 set RelayHeadpat false
python tools/fake_vrchat.py 192.168.1.50 tap RelayBoop
python tools/fake_vrchat.py 192.168.1.50 sweep RelayProximity
python tools/fake_vrchat.py 192.168.1.50 avatar             # simulate an avatar change
python tools/fake_vrchat.py listen                          # see what VRChat sends (no launch option)
```

`listen` is also the quickest way to find the exact parameter names your avatar sends.
Add `--port 9101` to send to the [PC twin](docs/pc-twin.md) instead.

## Relay modes and safety

| Mode     | Behaviour                                                       |
|----------|-----------------------------------------------------------------|
| `Follow` | Relay closed while the parameter is at or above `threshold`     |
| `Pulse`  | Relay closes for `pulseMs` on each activation                   |
| `Toggle` | Each activation flips the relay                                 |

- All relays open on boot, when the avatar changes, and when WiFi drops (the ESP32 can't hear
  "contact released" any more).
- `maxOnMs` forces a relay open after a time limit. Keep it set for anything that heats or moves.
- Float inputs have a small hysteresis so a Proximity contact hovering at the threshold doesn't
  make the relay chatter.
- `feedback` (optional) sends the relay state back to VRChat as a Bool parameter, so the avatar can
  show it. For this to work VRChat must accept packets from the LAN on port 9000, so you may need to
  allow VRChat through Windows Firewall.

## Wiring

Relay board `VCC` → 5V (VIN), `GND` → GND, `IN1..IN4` → the default pins in `config.h`:
GPIO 4, 5, 6, 7 on the ESP32-S3-DevKitC-1 (full table in the
[board guide](docs/esp32-s3-devkitc-1.md)), or GPIO 26, 27, 25, 33 on a generic ESP32. Most cheap relay boards are **active-low**
(`activeLow = true`). Some 5V boards don't switch reliably from 3.3V logic. If a relay won't release, use a board rated for 3.3V inputs or one
with a separate `JD-VCC` jumper.

⚠️ Mains voltage can kill you. Use enclosed, rated modules (or low-voltage loads) for anything
plugged into the wall.

## Project layout

| File | Purpose |
|------|---------|
| `include/config.h` | Ports, options and the relay table: the file you edit |
| `src/core/bridge.cpp` | Parameter handling, status page + JSON API, OSCQuery responses (shared) |
| `src/core/relays.cpp` | Relay modes, pulses, safety cutoff, active-low handling (shared) |
| `src/core/osc.cpp` | Small dependency-free OSC message/bundle parser and encoder (shared) |
| `src/platform/esp32/main.cpp` | ESP32: WiFi, UDP, web servers, mDNS, GPIO |
| `src/platform/native/main.cpp` | PC twin: Windows sockets, Windows mDNS, simulated GPIO |
| `docs/esp32-s3-devkitc-1.md` | Board guide: variants, USB ports, wiring, safe pins |
| `docs/pc-twin.md` | Running the project on a PC without hardware |
| `scripts/twin_targets.py` | Adds the "Run PC twin" tasks to the PlatformIO sidebar |
| `tools/fake_vrchat.py` | Send test parameters to the ESP32, or print incoming OSC |

HTTP API:
- `GET /api/state`: JSON `{"info":{...},"relays":[...],"params":[...]}`.
- `GET /api/events`: the same data as a live [Server-Sent Events](https://developer.mozilla.org/docs/Web/API/Server-sent_events)
  stream. It starts with a `state` event (everything), then sends `update` events with the relays and
  the parameters that changed. Relay changes go out immediately; parameter changes are batched every
  150 ms. The status page uses this and shows *live* / *reconnecting…* next to the title. It falls
  back to polling `/api/state` if the stream is refused. At most 2 streams on the ESP32 (3 on the
  PC twin); a new one replaces the oldest.
- `POST /api/relay?i=<index>&on=<0|1>`: manual switch; disable with `ENABLE_WEB_CONTROL`.

## Next steps

- Store the relay table in NVS/LittleFS and make it editable from the web page.
- Add authentication to the web controls.

## License

This project is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).
