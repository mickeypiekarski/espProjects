#pragma once

// Reusable OTA-update helper for ESP32 CYD boards, backed by GitHub Releases.
//
// Usage: copy this file into your sketch folder (Arduino IDE doesn't reliably
// resolve relative parent includes, so don't #include "../shared/..." — copy it).
// See README.md in shared/ota_update/ for the full setup checklist (partition
// scheme, cutting a release, the manifest.json convention, etc).
//
// A single GitHub Release can carry multiple named .bin assets (one per board
// type) plus a manifest.json describing them. Every call here that fetches an
// asset takes an explicit filename — nothing defaults to "the first asset."
//
// Before #include-ing this file, define:
//   FW_VERSION_CODE   integer version of THIS sketch, e.g. 10200 for v1.2.0
// Optionally override:
//   OTA_REPO               default "mickeypiekarski/espProjects"
//   OTA_CHECK_INTERVAL_MS  default 4 hours

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>

#ifndef FW_VERSION_CODE
#error "Define FW_VERSION_CODE (e.g. 10200 for v1.2.0) before including ota_update.h"
#endif

#ifndef OTA_REPO
#define OTA_REPO "mickeypiekarski/espProjects"
#endif

#ifndef OTA_CHECK_INTERVAL_MS
#define OTA_CHECK_INTERVAL_MS (4UL * 60 * 60 * 1000)
#endif

typedef void (*OtaStatusCallback)(const char* line1, const char* line2);

struct OtaCheckResult {
  bool checked = false;
  bool updateAvailable = false;
  int remoteVersionCode = -1;
};

// Parses a tag like "v1.2.0" into an integer version code 10200.
// Any parse failure returns -1 (treated as "no update").
inline int otaParseVersionCode(const String& tag) {
  int start = 0;
  while (start < (int)tag.length() && !isDigit(tag[start])) start++;
  int major = 0, minor = 0, patch = 0;
  int parsed = sscanf(tag.c_str() + start, "%d.%d.%d", &major, &minor, &patch);
  if (parsed < 1) return -1;
  return major * 10000 + minor * 100 + patch;
}

// Fetches the latest release's JSON (tag_name + assets[]) into doc.
// Returns false on any failure (no WiFi, network error, bad JSON) — caller
// should treat that as "nothing to do right now," not an error to surface loudly.
inline bool otaFetchLatestRelease(JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();  // no cert pinning — deliberate simplicity tradeoff, not an oversight

  HTTPClient http;
  String apiUrl = "https://api.github.com/repos/" OTA_REPO "/releases/latest";
  if (!http.begin(client, apiUrl)) return false;
  http.addHeader("User-Agent", "ESP32-OTA-Client");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  return deserializeJson(doc, payload) == DeserializationError::Ok;
}

// Finds assetFilename's browser_download_url within an already-fetched
// release JsonDocument. Returns "" if not present in this release.
inline String otaFindAssetUrl(JsonDocument& releaseDoc, const char* assetFilename) {
  JsonArray assets = releaseDoc["assets"].as<JsonArray>();
  for (JsonObject asset : assets) {
    String name = asset["name"] | "";
    if (name == assetFilename) {
      return asset["browser_download_url"] | "";
    }
  }
  return "";
}

// Fetches manifest.json from the latest release's assets and parses it into
// `out`. Returns false on any failure (network, missing manifest.json asset,
// parse error) — caller should show an error/empty state, not retry-loop.
inline bool otaFetchManifest(JsonDocument& out) {
  JsonDocument releaseDoc;
  if (!otaFetchLatestRelease(releaseDoc)) return false;

  String manifestUrl = otaFindAssetUrl(releaseDoc, "manifest.json");
  if (manifestUrl.length() == 0) return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, manifestUrl)) return false;
  http.addHeader("User-Agent", "ESP32-OTA-Client");

  // manifest.json is a GitHub release asset too, so the same redirect gotcha
  // applies here as it does to .bin downloads below.
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  String payload = http.getString();
  http.end();

  return deserializeJson(out, payload) == DeserializationError::Ok;
}

// Unconditionally finds assetFilename in the latest release and flashes it —
// no version comparison. This is what the installer uses to flash a chosen
// board type's binary; it's also the primitive checkForOTAUpdate() below
// builds on once its own version gate has passed.
inline t_httpUpdate_return otaFlashAsset(const char* assetFilename, OtaStatusCallback statusCallback = nullptr) {
  JsonDocument releaseDoc;
  if (!otaFetchLatestRelease(releaseDoc)) return HTTP_UPDATE_FAILED;

  String assetUrl = otaFindAssetUrl(releaseDoc, assetFilename);
  if (assetUrl.length() == 0) return HTTP_UPDATE_NO_UPDATES;

  if (statusCallback) statusCallback("Updating...", "Do not power off");

  // GOTCHA: GitHub asset URLs 302-redirect to an S3-backed CDN URL.
  // HTTPUpdate does not follow redirects by default — without this line,
  // downloads fail. Do not remove.
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  WiFiClientSecure updateClient;
  updateClient.setInsecure();
  t_httpUpdate_return ret = httpUpdate.update(updateClient, assetUrl);

  if (ret == HTTP_UPDATE_FAILED && statusCallback) {
    statusCallback("Update failed", "Continuing...");
  }
  // HTTP_UPDATE_OK: device reboots itself; execution should not reach here.
  return ret;
}

// Checks GitHub Releases for a newer build of THIS sketch's own asset, and
// flashes it if found. On success, the device reboots itself — this function
// does not return in that branch. On any failure (network, parse, no newer
// version, asset not in this release), it returns cleanly with
// checked=false/updateAvailable=false; caller should just continue normal
// boot. Never blocks, retries, or crashes on failure.
inline OtaCheckResult checkForOTAUpdate(const char* assetFilename, OtaStatusCallback statusCallback = nullptr) {
  OtaCheckResult result;

  JsonDocument releaseDoc;
  if (!otaFetchLatestRelease(releaseDoc)) return result;

  result.checked = true;

  String tagName = releaseDoc["tag_name"] | "";
  int remoteVersion = otaParseVersionCode(tagName);
  result.remoteVersionCode = remoteVersion;
  if (remoteVersion <= FW_VERSION_CODE) return result;

  if (otaFindAssetUrl(releaseDoc, assetFilename).length() == 0) return result;

  result.updateAvailable = true;
  if (statusCallback) statusCallback("Update found", tagName.c_str());

  otaFlashAsset(assetFilename, statusCallback);
  return result;
}
