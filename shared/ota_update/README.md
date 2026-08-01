# OTA Update Helper

Reusable GitHub-Releases-backed OTA updater for CYD boards, paired with:
- `board_template.ino.example` — scaffold for a new **standalone board type**
  (subway, surf, weather, ...). Each board type compiles to its own
  independent binary.
- `installer/` (at the repo root, not in this folder) — the *only* firmware
  ever flashed over USB. It lets a friend pick a board type from a web page
  and OTA-installs that type's binary. See "Installer" below.

A board becomes "a subway board" (or surf, or weather) by being flashed with
that type's binary — either via the installer, or directly over USB during
development. There's no in-app mode picker anymore; the board type *is* the
binary.

## Setting up a new board type

1. Create `espProjects/<board_name>/`.
2. Copy `ota_update.h` and `board_template.ino.example` into that folder,
   renaming the `.ino.example` to `<board_name>.ino`.
   - Copy, don't reference: Arduino IDE doesn't reliably resolve relative
     parent includes (`#include "../shared/ota_update/ota_update.h"`), so
     each board folder needs its own copy. `shared/ota_update/` is the
     canonical source — re-copy `ota_update.h` here if it's ever updated.
3. Set `BOARD_NAME`, `MDNS_HOSTNAME`, `FW_VERSION_CODE`, and
   `BOARD_ASSET_NAME` at the top of the sketch. Once WiFi is connected, the
   board is reachable at `http://<MDNS_HOSTNAME>.local` — no need to find its
   IP via Serial/router. `MDNS_HOSTNAME` must be lowercase with no spaces
   (e.g. `"subwayboard"`). `BOARD_ASSET_NAME` is the `.bin` filename this
   board's OTA checks look for in each release — convention is
   `"<board_name>.bin"`, matching the folder/sketch name. Most phones/laptops
   resolve `.local` automatically; a small minority of older Android devices
   don't support mDNS and would need the raw IP instead (visible over Serial
   as a fallback).
4. Implement your board's actual logic directly in the `.ino` (replace the
   `setupBoard()`/`loopBoard()` stubs) — any on-device config web page it
   needs (like `subway_board`'s station picker), `Preferences` for persisted
   state, TFT drawing, etc. There's no mode-dispatch system to plug into
   anymore; one sketch, one board type, one binary.
5. **Arduino IDE → Tools → Partition Scheme**: must be set to an OTA-capable
   scheme (one with two app partitions), not a single-app-partition scheme.
   This is a per-board, per-machine manual setting — code can't enforce it.
   If unsure which scheme is OTA-capable on your board package version,
   check the partition table comment shown in the IDE's scheme dropdown;
   look for one explicitly labeled "OTA" or with two `app` partitions.
6. Once you're ready to make this board type installable, add it to a
   release's `manifest.json` (see "Cutting a release" below) so it shows up
   in the installer's picker page.

## Cutting a release

A single GitHub Release can contain **multiple board types' binaries at
once**, all sharing one version tag. To publish an update:

1. For each board type you're updating: Arduino IDE → **Sketch → Export
   Compiled Binary**, find the exported `.bin` in that board's sketch folder.
2. Write (or update) `manifest.json` — a JSON array describing every board
   type that should be installable from this release:
   ```json
   [
     {"name": "Subway (G train)", "asset": "subway_board.bin"},
     {"name": "Surf",              "asset": "surf_board.bin"}
   ]
   ```
   `name` is the friendly label shown on the installer's picker page; `asset`
   must exactly match that board's `BOARD_ASSET_NAME` / uploaded filename.
   Carry forward every existing entry even if you're only updating one board
   type — this file is the installer's entire view of what's installable.
3. On GitHub: **Releases → Draft a new release**, tag it `vX.Y.Z` (must match
   the `FW_VERSION_CODE` you bumped in each updated sketch, e.g. tag `v1.2.0`
   ↔ `FW_VERSION_CODE 10200`). This one tag/version governs every board
   type's version comparison in this release.
4. Attach every board type's `.bin` (named per its `BOARD_ASSET_NAME`) plus
   `manifest.json` as release assets, publish.
5. Already-running boards check for updates on boot and every
   `OTA_CHECK_INTERVAL_MS` (default 4 hours) — each only looks for its own
   named asset, so a release that only changes one board type doesn't affect
   any other board type's boards.
6. Already-flashed **installers** pick up new/changed `manifest.json` entries
   the next time someone loads their picker page — no installer re-flash
   needed, ever, even for installers already sold and out in the field.

## How the version check works

- `GET https://api.github.com/repos/<OTA_REPO>/releases/latest` (public,
  unauthenticated — 60 requests/hour/IP is far more than a periodic check
  needs).
- Compares the release tag's version against the sketch's compiled-in
  `FW_VERSION_CODE`. Only proceeds if the release is newer.
- Downloads the asset whose name matches this sketch's own
  `BOARD_ASSET_NAME` via `HTTPUpdate`, which handles the flash write,
  integrity check, and reboot. If no asset in the release matches this
  board's name, it's treated as "nothing to do" — not an error — since a
  release may only be updating other board types.

## Installer

`installer/installer.ino` (repo root) is the one sketch ever flashed to a
board over USB. It's intentionally minimal: WiFiManager setup, a self-OTA
check (so the installer itself can be patched without a re-flash), and a web
picker page built from `manifest.json` in the latest release. Picking a
board type calls `otaFlashAsset()` to download and flash that type's binary
unconditionally (no version gate — it's installing a different binary
outright, not self-updating), then reboots into it.

Because boards are sold and can never be physically re-flashed after that
first USB install, the installer's board-type list is **not hardcoded** — it
always reflects whatever `manifest.json` currently says, fetched fresh each
time the picker page loads. Adding a wholly new board type later is purely a
release-side change (new `.bin` + updated `manifest.json`); every installer
already in someone's hands picks it up automatically.

Setup follows the same steps as any board (copy `ota_update.h` in, OTA-capable
partition scheme required, `FW_VERSION_CODE`/`MDNS_HOSTNAME` at the top) —
its `BOARD_ASSET_NAME` is `"installer.bin"`.

## Known limitations (deliberate, not oversights)

- **Repo must stay public.** The version check hits the GitHub Releases API
  unauthenticated (no token baked into firmware). Against a private repo
  this 404s indistinguishably from "no releases exist" — no error, the
  board just silently never updates. If this repo is ever made private,
  the OTA flow needs a token added (and the "no extra auth" trust model
  reconsidered).
- **No TLS certificate validation** (`WiFiClientSecure::setInsecure()`).
  Keeps things simple for a personal project; would need revisiting if this
  pattern is ever reused somewhere security-sensitive.
- **No rollback safety net.** If a released binary is broken and crash-loops
  after flashing, there's no automatic recovery — only a manual USB re-flash.
  ESP-IDF has app-rollback support, but it needs partition/Kconfig changes
  beyond what Arduino IDE's GUI exposes, plus an explicit
  "mark valid after confirming healthy operation" step in the sketch. Given
  the board's existing reset gestures already assume USB access is available
  as a fallback, this is deferred rather than solved here. This matters more
  now that boards are sold — a bad release could brick devices with no
  remote recovery path — so keep this in mind before pushing to production
  releases.
- **`manifest.json` has no schema validation on-device.** A malformed
  `manifest.json` just fails to parse and the installer shows an error page;
  it doesn't crash or brick anything, but double-check the JSON is valid
  before publishing a release.
