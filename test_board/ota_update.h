#pragma once

// Reusable OTA-update helper for ESP32 CYD boards, backed by GitHub Releases.
//
// Usage: copy this file into your sketch folder (Arduino IDE doesn't reliably
// resolve relative parent includes, so don't #include "../shared/..." — copy it).
// See README.md in shared/ota_update/ for the full setup checklist (partition
// scheme, cutting a release, etc).
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

// Checks GitHub Releases for a newer build, and flashes it if found.
// On success, httpUpdate.update() reboots the device itself — this function
// does not return in that branch. On any failure (network, parse, flash),
// it returns cleanly with checked=false/updateAvailable=false; caller should
// just continue normal boot. Never blocks, retries, or crashes on failure.
inline OtaCheckResult checkForOTAUpdate(OtaStatusCallback statusCallback = nullptr) {
  OtaCheckResult result;

  if (WiFi.status() != WL_CONNECTED) return result;

  WiFiClientSecure client;
  client.setInsecure();  // no cert pinning — deliberate simplicity tradeoff, not an oversight

  HTTPClient http;
  String apiUrl = "https://api.github.com/repos/" OTA_REPO "/releases/latest";
  if (!http.begin(client, apiUrl)) return result;
  http.addHeader("User-Agent", "ESP32-OTA-Client");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return result;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return result;

  result.checked = true;

  String tagName = doc["tag_name"] | "";
  int remoteVersion = otaParseVersionCode(tagName);
  result.remoteVersionCode = remoteVersion;
  if (remoteVersion <= FW_VERSION_CODE) return result;

  String assetUrl;
  JsonArray assets = doc["assets"].as<JsonArray>();
  if (assets.size() > 0) {
    assetUrl = assets[0]["browser_download_url"] | "";
  }
  if (assetUrl.length() == 0) return result;

  result.updateAvailable = true;
  if (statusCallback) statusCallback("Update found", tagName.c_str());

  // GOTCHA: GitHub asset URLs 302-redirect to an S3-backed CDN URL.
  // HTTPUpdate does not follow redirects by default — without this line,
  // downloads fail. Do not remove.
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  if (statusCallback) statusCallback("Updating...", "Do not power off");

  WiFiClientSecure updateClient;
  updateClient.setInsecure();
  t_httpUpdate_return ret = httpUpdate.update(updateClient, assetUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      if (statusCallback) statusCallback("Update failed", "Continuing...");
      break;
    case HTTP_UPDATE_NO_UPDATES:
      break;
    case HTTP_UPDATE_OK:
      // Device reboots itself on success; execution should not reach here.
      break;
  }

  return result;
}
