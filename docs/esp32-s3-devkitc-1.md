# ESP32-S3-DevKitC-1

Notes for running this project on Espressif's
[ESP32-S3-DevKitC-1 v1.0](https://docs.espressif.com/projects/esp-idf/en/v5.0.1/esp32s3/hw-reference/esp32s3/user-guide-devkitc-1-v1.0.html).
It is the default PlatformIO environment (`esp32-s3-devkitc-1`), so `pio run -t upload` builds for it.

## Which variant do you have?

The ordering code is on the box and on the module's metal shield.

| Variant | Module | Flash / PSRAM | What to change |
|---|---|---|---|
| N8 | WROOM-1 | 8 MB quad / none | nothing |
| N8R2 | WROOM-1 | 8 MB quad / 2 MB quad | nothing |
| N8R8 | WROOM-1 | 8 MB quad / 8 MB octal | nothing, but GPIO 35–37 are taken by PSRAM |
| N16R8V, N32R8V | WROOM-2 | 16/32 MB octal / 8 MB octal | add `board_build.arduino.memory_type = opi_opi` to the env in `platformio.ini`; GPIO 35–37 are taken |

This project doesn't use PSRAM, and 8 MB of flash is plenty on every variant.

## USB ports

The board has two Micro-USB ports:

- **UART** (USB-to-UART bridge chip): use this one. Flashing and `pio device monitor` both work
  through it, because the firmware prints to UART0 (GPIO 43/44).
- **USB** (the ESP32-S3's native USB, GPIO 19/20): it can flash the board too, but the serial log
  won't show up there unless you add `-DARDUINO_USB_CDC_ON_BOOT=1` to `build_flags`.

If uploading fails, hold **BOOT**, press **RST**, release **BOOT**, and upload again.

## Wiring a 4-channel relay board

The defaults in `include/config.h` are all on header **J1**, the left header with the USB ports
facing down:

| Relay board | DevKitC-1 (J1) | Relay in `config.h` |
|---|---|---|
| IN1 | GPIO 4 (pin 4) | `RelayHeadpat` |
| IN2 | GPIO 5 (pin 5) | `RelayBoop` |
| IN3 | GPIO 6 (pin 6) | `RelayHandToggle` |
| IN4 | GPIO 7 (pin 7) | `RelayProximity` |
| VCC | 5V (pin 21) | |
| GND | G (pin 22) | |

The 5V pin carries USB power, so relay coils are fine while the board runs from USB. The
active-low and 3.3 V logic notes in the main README apply here too.

## Choosing other pins

| GPIO | Use for relays? | Why |
|---|---|---|
| 1, 2, 4–18, 21, 47 | ✅ yes | general purpose |
| 39–42 | ✅ yes | JTAG pins, free unless you debug over JTAG |
| 0, 3, 45, 46 | ❌ avoid | strapping pins: a relay board pulling them can stop the board from booting or change how it boots |
| 19, 20 | ❌ avoid | native USB D−/D+ |
| 43, 44 | ❌ avoid | UART0 TX/RX, used by flashing and the serial monitor |
| 35, 36, 37 | ❌ on R8 / WROOM-2 | wired to octal PSRAM/flash inside the module |
| 48 | ❌ avoid | onboard RGB LED (WS2812) on v1.0 boards |
| 38 | ⚠️ check | onboard RGB LED on v1.1 boards; free on v1.0 |
| 22–34 | — | not on the headers (26–32 are internal flash pins) |

Change `RELAY_PIN_1` to `RELAY_PIN_4` in the `CONFIG_IDF_TARGET_ESP32S3` block of `config.h`. Add
more rows to `RELAYS` if you need more relays (up to 16).

## WiFi

The ESP32-S3 only supports 2.4 GHz WiFi, like the classic ESP32. Nothing else changes: OSCQuery,
the status page and `tools/fake_vrchat.py` work the same on both boards.
