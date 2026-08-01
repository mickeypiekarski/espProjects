# ESP32 CYD Projects

A collection of projects for the **ESP32 Cheap Yellow Display (CYD)** — an ESP32 development board with a built-in 2.8" TFT touchscreen.

## Hardware

- **Board:** ESP32-2432S028R (a.k.a. "Cheap Yellow Display" / CYD)
- **Display:** 2.8" ILI9341 TFT (320×240), SPI
- **Touch:** XPT2046 resistive touch controller
- **WiFi/BT:** ESP32 onboard

## Projects

| Folder | Description |
|--------|-------------|
| `subway_project/` | NYC subway arrival boards for various stops (G train Greenpoint, A/C Utica Av), plus a Dodgers score panel and a cycling news ticker |
| `shared/ota_update/` | Reusable OTA-update helper + multi-mode board template (see [CLAUDE.md](CLAUDE.md#ota-updates)) — new boards remote-update over WiFi via GitHub Releases instead of requiring a USB re-flash |

## Architecture

Live MTA data is served through a custom proxy:

**[mta-proxy](https://github.com/mickeypiekarski/mta-proxy)** — a Flask app deployed on Render that:
- Fetches GTFS-RT protobuf feeds directly from the MTA API
- Parses arrivals per stop/direction and returns clean JSON
- Also exposes a `/dodgers` endpoint (MLB Stats API)

The CYD sketches call the proxy over WiFi using `HTTPClient` and parse the response with `ArduinoJson`. Refresh interval is typically 30s for trains.

> **Note:** The proxy is on Render's free tier — the first request after idle may take 10–30s to cold-start.

## Setup

### Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) 2.x
- [ESP32 Arduino core](https://github.com/espressif/arduino-esp32) via Boards Manager
  - URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`

### Board Settings (Arduino IDE)

| Setting | Value |
|---------|-------|
| Board | ESP32 Dev Module |
| Upload Speed | 921600 |
| Flash Size | 4MB |
| Partition Scheme | Default 4MB with spiffs |
| PSRAM | Disabled |

> **OTA note:** boards using `shared/ota_update/` need an OTA-capable partition scheme (two app partitions). Verify "Default 4MB with spiffs" actually provides that on your installed core version before relying on it — see `shared/ota_update/README.md`.

### Common Libraries

- **TFT_eSPI** — display driver (configure `User_Setup.h` for ILI9341 + CYD pinout)
- **XPT2046_Touchscreen** — touch input
- **ArduinoJson** — JSON parsing for proxy responses
- **WiFiManager** — captive-portal WiFi setup (used in newer sketches)

## License

MIT
