# ESP32 CYD Projects — Claude Context

## What this repo is

Arduino-based projects for the ESP32 Cheap Yellow Display (CYD) board. Each project lives in its own subfolder.

## Hardware

- **MCU:** ESP32 (dual-core Xtensa LX6, 240 MHz, 520 KB SRAM)
- **Display:** 2.8" ILI9341 TFT 320×240 via SPI
- **Touch:** XPT2046 resistive controller
- **Flash:** 4 MB
- **WiFi:** 802.11 b/g/n onboard

## Toolchain

- **IDE:** Arduino IDE 2.x
- **Core:** arduino-esp32
- **Key libraries:** TFT_eSPI, XPT2046_Touchscreen

## CYD Pin Reference

| Function | GPIO |
|----------|------|
| TFT MOSI | 13 |
| TFT MISO | 12 |
| TFT CLK  | 14 |
| TFT CS   | 15 |
| TFT DC   | 2  |
| TFT BL   | 21 |
| Touch CS | 33 |
| Touch IRQ| 36 |
| SD CS    | 5  |

## Backend / Proxy

All live data goes through **mta-proxy** (https://github.com/mickeypiekarski/mta-proxy), a Flask app on Render:

- `GET /arrivals?stop=<GTFS_STOP_ID>&feed=<feed>` — returns JSON with `directions[]`, each with a `label` and `arrivals[]` (fields: `mins`, `route`, `terminal`)
- `GET /dodgers` — MLB Stats API wrapper; returns score, R/H/E, K, LOB, OPS, top batters, winning pitcher
- `GET /` — legacy endpoint, hardcoded to G train at Greenpoint southbound; returns `arrivals[]` as plain int array

Feed IDs: `ace`, `bdfm`, `g`, `jz`, `nqrw`, `l`, `123456`, `7`, `sir`

**Render free tier**: first request after idle cold-starts in 10–30s. Sketches should handle timeouts gracefully.

## WiFi setup patterns

Two patterns in use across sketches:
1. **Manual scan + touch keyboard** (`gtrain.ino`) — scans networks, shows scrollable list, on-screen keyboard for password, saves to `Preferences`
2. **WiFiManager captive portal** (`utica_board.ino`, `g_train_board.ino`) — creates AP `"<BoardName>-Setup"`, user connects and visits `192.168.4.1`

## OTA updates

New boards should support remote firmware updates instead of requiring a USB
re-flash. `shared/ota_update/` has the reusable pieces:

- `ota_update.h` — GitHub-Releases-backed OTA checker/updater, copy into each
  board's sketch folder (don't relative-`#include` across folders — Arduino
  IDE doesn't reliably resolve that)
- `board_template.ino.example` — scaffold for new boards: WiFiManager setup
  → OTA check → on-device mode-picker web page → mode dispatch
- `README.md` — setup checklist and how to cut a release

One firmware binary serves all board "modes" (subway, surf, weather, ...);
the active mode is chosen via a web page on the device and stored in
`Preferences`, not selected via separate per-mode binaries. Releases are
tagged `vX.Y.Z` on GitHub (repo: espProjects) with a single `.bin` asset.

**Arduino IDE → Tools → Partition Scheme must be OTA-capable** (two app
partitions) for any board using this — a per-machine manual setting, not
something the code can enforce.

**This repo is public.** `ota_update.h` calls the GitHub Releases API
unauthenticated (no token baked into firmware, by design — see
"Known limitations" in `shared/ota_update/README.md`), and GitHub's
unauthenticated API 404s on private repos indistinguishably from "no
releases." The repo must stay public for OTA to keep working. End-to-end
tested 2026-08-01: v1.0.0 → v1.0.1 self-update over WiFi confirmed on
physical hardware.

## Conventions

- One Arduino sketch (`.ino`) per project folder, folder name matches sketch name
- No hardcoded WiFi credentials — use WiFiManager or `Preferences`
- Use `TFT_eSPI` for all display drawing; avoid mixing display libraries
- Refresh trains every 30s (`FETCH_INTERVAL 30000`), slower data (Dodgers, weather) every 5min+

## What to watch out for

- `TFT_eSPI` requires editing `User_Setup.h` in the library folder — document any non-default values in the project README
- The CYD's USB-serial chip is CH340; drivers may be needed on macOS/Windows
- GPIO 34–39 are input-only on ESP32 — don't assign outputs there (touch MISO is on 39, touch IRQ on 36 — both fine as inputs)
- PSRAM is not present on the standard CYD variant
- Touch uses a **separate VSPI bus** (CLK=25, MISO=39, MOSI=32, CS=33, IRQ=36) — initialize with `SPIClass touchSPI(VSPI)` before `ts.begin(touchSPI)`
- `ArduinoJson` v7+ uses `JsonDocument` (not `DynamicJsonDocument`) — existing code already uses this
