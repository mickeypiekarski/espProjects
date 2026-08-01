// installer.ino
//
// The ONLY firmware ever flashed to a board over USB. Every physical board
// gets exactly one USB flash, ever — after that it must be fully
// remote-manageable, including being told what type of board it is.
//
// Flow: WiFiManager setup -> self-OTA check -> fetch manifest.json from the
// latest GitHub Release -> show a picker page listing board types -> on
// selection, flash that type's .bin and reboot into it. From that point on,
// this device runs the chosen board's own firmware and its own OTA loop —
// the installer's job for this physical unit is over permanently.
//
// Board types are NOT hardcoded here. They come from manifest.json, a JSON
// array asset attached to each GitHub Release, e.g.:
//   [{"name": "Subway (G train)", "asset": "subway_board.bin"}]
// Adding a new board type later means publishing a new release with an
// updated manifest.json — zero installer changes, zero re-flash, works on
// installers already sold and in the field. See shared/ota_update/README.md.

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>

#define BOARD_NAME "Installer"
#define MDNS_HOSTNAME "install"          // reachable at http://install.local
#define FW_VERSION_CODE 10000            // v1.0.0 — the installer's own version line
#define BOARD_ASSET_NAME "installer.bin"

#include "ota_update.h"

TFT_eSPI tft = TFT_eSPI();
WiFiManager wifiManager;
WebServer installServer(80);

unsigned long lastOtaCheck = 0;

void showMessage(const char* line1, const char* line2) {
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(10, 10);
  tft.println(line1);
  tft.setCursor(10, 30);
  tft.println(line2);
}

// ---- Board-type picker web UI -----------------------------------------

void handlePickerRoot() {
  JsonDocument manifest;
  if (!otaFetchManifest(manifest)) {
    installServer.send(503, "text/html",
      "<html><body><h2>Couldn't load board list</h2>"
      "<p>Check your internet connection and reload.</p></body></html>");
    return;
  }

  String page = "<html><body><h2>" BOARD_NAME "</h2>"
                "<p>Pick what kind of board this is:</p><ul>";
  JsonArray types = manifest.as<JsonArray>();
  for (JsonObject type : types) {
    String name = type["name"] | "";
    String asset = type["asset"] | "";
    if (name.length() == 0 || asset.length() == 0) continue;
    page += "<li><a href=\"/install?asset=" + asset + "\">" + name + "</a></li>";
  }
  page += "</ul></body></html>";

  installServer.send(200, "text/html", page);
}

void handleInstall() {
  if (!installServer.hasArg("asset")) {
    installServer.send(400, "text/plain", "Missing asset param");
    return;
  }
  String asset = installServer.arg("asset");
  installServer.send(200, "text/html",
    "<html><body><h2>Installing " + asset + "...</h2>"
    "<p>The board will restart shortly.</p></body></html>");

  showMessage("Installing...", asset.c_str());
  otaFlashAsset(asset.c_str(), showMessage);
  // If we reach here, the flash failed (device reboots itself on success).
  showMessage("Install failed", "Reload the page to retry");
}

void setup() {
  Serial.begin(115200);
  Serial.printf("Running FW_VERSION_CODE: %d\n", FW_VERSION_CODE);
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  wifiManager.setAPCallback([](WiFiManager*) {
    showMessage("Connect to WiFi:", BOARD_NAME "-Setup");
  });
  if (!wifiManager.autoConnect(BOARD_NAME "-Setup")) {
    ESP.restart();
  }

  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.printf("mDNS ready: http://%s.local\n", MDNS_HOSTNAME);
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("mDNS setup failed — fall back to IP from Serial output");
  }

  showMessage("Checking for", "updates...");
  checkForOTAUpdate(BOARD_ASSET_NAME, showMessage);  // reboots in-place if the installer itself has a newer build

  installServer.on("/", handlePickerRoot);
  installServer.on("/install", handleInstall);
  installServer.begin();

  showMessage("Ready!", "Visit http://" MDNS_HOSTNAME ".local");
}

void loop() {
  installServer.handleClient();

  if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
    lastOtaCheck = millis();
    checkForOTAUpdate(BOARD_ASSET_NAME, showMessage);
  }
}
