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

Boards are **sold** — each physical unit gets exactly one USB flash, ever.
After that it must be fully remote-manageable, including being told what
*type* of board it is. The architecture:

- **`installer/`** — the only firmware ever flashed over USB. WiFiManager
  setup → self-OTA check → web page listing available board types (fetched
  from `manifest.json` in the latest release, not hardcoded) → picking one
  downloads and flashes that type's own binary, then reboots into it.
- **Standalone per-board-type sketches** (`subway_board/`, future
  `surf_board/`, `weather_board/`, ...) — each is a complete, independent
  binary with its own `FW_VERSION_CODE` and its own OTA check scoped to its
  own named release asset. No in-binary mode picker; the board type *is* the
  binary.
- **`shared/ota_update/`** — the reusable pieces:
  - `ota_update.h` — GitHub-Releases-backed OTA helper, copy into each
    board's sketch folder (don't relative-`#include` across folders —
    Arduino IDE doesn't reliably resolve that). Provides
    `checkForOTAUpdate(assetFilename, cb)` (version-gated self-update against
    a named asset), `otaFlashAsset(assetFilename, cb)` (unconditional flash —
    what the installer uses), and `otaFetchManifest(doc)`.
  - `board_template.ino.example` — scaffold for a new standalone board type.
  - `README.md` — setup checklist, how to cut a release, the `manifest.json`
    convention, and the installer's design.

Releases are tagged `vX.Y.Z` on GitHub (repo: espProjects) and can carry
**multiple named `.bin` assets at once** (one per board type currently being
updated, named `<board_name>.bin`) plus a `manifest.json` asset listing
`{name, asset}` pairs for every currently-installable board type. Adding a
brand-new board type is purely a release-side change — no installer
re-flash, ever, even for installers already sold and in someone's hands.

**Arduino IDE → Tools → Partition Scheme must be OTA-capable** (two app
partitions) for any board using this — a per-machine manual setting, not
something the code can enforce.

**This repo is public.** `ota_update.h` calls the GitHub Releases API
unauthenticated (no token baked into firmware, by design — see
"Known limitations" in `shared/ota_update/README.md`), and GitHub's
unauthenticated API 404s on private repos indistinguishably from "no
releases." The repo must stay public for OTA to keep working. The underlying
OTA mechanics (redirect-follow, insecure TLS, integer version compare) were
hardware-validated 2026-08-01 (v1.0.0 → v1.0.1 self-update over WiFi) under
the earlier one-binary/mode-picker design; the installer + per-board-type
flow built on top of those same mechanics has not yet been hardware-tested
end-to-end.

## TODO: flash headroom is tighter than expected

Splitting into per-board-type binaries (installer + `subway_board`, etc.)
fixed the architectural problem — a board no longer carries other board
types' code — but did **not** free up much flash. Compiled via `arduino-cli`
against `esp32:esp32:esp32` (Default 4MB with spiffs, 1.25MB per app
partition):
- `subway_board/` — 95% full (1,251,420 / 1,310,720 bytes)
- `installer/` — 94% full (1,241,132 / 1,310,720 bytes), despite having
  almost no board-specific logic

`nm -S --size-sort` on the compiled `.elf` shows the bulk is fixed overhead
from the WiFi/TLS/lwIP stack itself (`mbedtls_ssl_handshake_*`, `tcp_input`,
WebServer request parsing, etc.) — cost every board pays just for using
WiFiManager + HTTPS OTA + WebServer, largely independent of board-specific
logic size. Realistic headroom for a single board's own code is more like
~60–70KB, not the "well under 95%" the per-binary split was expected to buy.

**Revisit before building heavier board types** (more graphics, more
sensors, bigger data tables than the 12-station G-line list): look at a
partition scheme with a larger app slot (4MB flash isn't otherwise close to
full — SPIFFS/FATFS space is going mostly unused) or trimming library
footprint (e.g. dropping WiFiManager's captive-portal HTML/JS if it's ever
swappable for something lighter).

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
