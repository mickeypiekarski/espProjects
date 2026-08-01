# OTA Update Helper

Reusable GitHub-Releases-backed OTA updater for CYD boards, paired with a
multi-mode board template so one firmware can serve as subway board, surf
board, weather board, etc. — the user picks the active mode from a web page
served by the device itself, no separate binary per board.

## Setting up a new board

1. Create `espProjects/<board_name>/`.
2. Copy `ota_update.h` and `board_template.ino.example` into that folder,
   renaming the `.ino.example` to `<board_name>.ino`.
   - Copy, don't reference: Arduino IDE doesn't reliably resolve relative
     parent includes (`#include "../shared/ota_update/ota_update.h"`), so
     each board folder needs its own copy. `shared/ota_update/` is the
     canonical source — re-copy `ota_update.h` here if it's ever updated.
3. Set `BOARD_NAME`, `MDNS_HOSTNAME`, and `FW_VERSION_CODE` at the top of the
   sketch. Once WiFi is connected, the board is reachable at
   `http://<MDNS_HOSTNAME>.local` — no need to find its IP via Serial/router.
   `MDNS_HOSTNAME` must be lowercase with no spaces (e.g. `"subwayboard"`).
   Most phones/laptops resolve `.local` automatically; a small minority of
   older Android devices don't support mDNS and would need the raw IP
   instead (visible over Serial as a fallback).
4. Add a `mode_<name>.h` per mode you want the board to support (subway,
   surf, weather, ...), with `setup_<mode>()` / `loop_<mode>()` functions,
   and register it in the mode enum + `dispatchMode()`/`loop()` switch.
5. **Arduino IDE → Tools → Partition Scheme**: must be set to an OTA-capable
   scheme (one with two app partitions), not a single-app-partition scheme.
   This is a per-board, per-machine manual setting — code can't enforce it.
   If unsure which scheme is OTA-capable on your board package version,
   check the partition table comment shown in the IDE's scheme dropdown;
   look for one explicitly labeled "OTA" or with two `app` partitions.

## Cutting a release

1. Arduino IDE: **Sketch → Export Compiled Binary**.
2. Find the exported `.bin` in the sketch folder.
3. On GitHub: **Releases → Draft a new release**, tag it `vX.Y.Z` (must match
   the `FW_VERSION_CODE` you bumped in the sketch, e.g. tag `v1.2.0` ↔
   `FW_VERSION_CODE 10200`).
4. Attach the `.bin` as a release asset, publish.
5. Boards check for updates on boot and every `OTA_CHECK_INTERVAL_MS`
   (default 4 hours) — no further action needed.

## How the version check works

- `GET https://api.github.com/repos/<OTA_REPO>/releases/latest` (public,
  unauthenticated — 60 requests/hour/IP is far more than a periodic check
  needs).
- Compares the release tag's version against the sketch's compiled-in
  `FW_VERSION_CODE`. Only downloads if the release is newer.
- Downloads the first asset's `browser_download_url` via `HTTPUpdate`, which
  handles the flash write, integrity check, and reboot.

## Known limitations (deliberate, not oversights)

- **No TLS certificate validation** (`WiFiClientSecure::setInsecure()`).
  Keeps things simple for a personal project; would need revisiting if this
  pattern is ever reused somewhere security-sensitive.
- **No rollback safety net.** If a released binary is broken and crash-loops
  after flashing, there's no automatic recovery — only a manual USB re-flash.
  ESP-IDF has app-rollback support, but it needs partition/Kconfig changes
  beyond what Arduino IDE's GUI exposes, plus an explicit
  "mark valid after confirming healthy operation" step in the sketch. Given
  the board's existing reset gestures already assume USB access is available
  as a fallback, this is deferred rather than solved here.
- **One asset per release.** The mode picker means one firmware serves every
  board type, so there's no need for per-board-type binaries or naming
  conventions — keep releases to a single `.bin` asset.
