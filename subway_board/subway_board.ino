// subway_board.ino
//
// Standalone G-train arrivals board. WiFiManager setup -> OTA check (scoped
// to this board's own release asset) -> station picker web page -> live
// arrivals display, refreshed every 30s from mta-proxy.
//
// This is a complete, independent binary — not a mode inside a larger
// picker. Boards become "a subway board" by being flashed with THIS file via
// the installer (see espProjects/installer/), not by picking a mode at
// runtime. See shared/ota_update/board_template.ino.example for the scaffold
// this was built from, and shared/ota_update/README.md for how to cut a
// release with this board's .bin attached.

#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define BOARD_NAME "SubwayBoard"
#define MDNS_HOSTNAME "subwayboard"       // reachable at http://subwayboard.local
#define FW_VERSION_CODE 10000             // v1.0.0 — bump on every release
#define BOARD_ASSET_NAME "subway_board.bin"

#include "ota_update.h"

TFT_eSPI tft = TFT_eSPI();
WiFiManager wifiManager;
WebServer webServer(80);
Preferences prefs;

unsigned long lastOtaCheck = 0;

// ---- Station database (G line only) ------------------------------------

#define SCREEN_W 320
#define SCREEN_H 240
#define HEADER_H 34
#define FETCH_INTERVAL 30000

const char* PROXY_BASE = "https://mta-proxy.onrender.com/arrivals";

#define C_G 0x65E8  // #6CBE45 MTA G-train green (RGB565)

struct Station { const char* name; const char* stopId; };

const Station STATIONS[] = {
  {"Court Sq",               "G22"},
  {"21 St",                  "G24"},
  {"Greenpoint Av",          "G26"},
  {"Nassau Av",              "G28"},
  {"Metropolitan Av",        "G29"},
  {"Broadway",               "G30"},
  {"Flushing Av",            "G31"},
  {"Myrtle-Willoughby Avs",  "G32"},
  {"Bedford-Nostrand Avs",   "G33"},
  {"Classon Av",             "G34"},
  {"Clinton-Washington Avs", "G35"},
  {"Fulton St",              "G36"},
};
const int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);

String savedStopId = "";
String savedStopName = "";
unsigned long lastFetch = 0;

void showMessage(const char* line1, const char* line2) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  tft.drawString(line1, 10, 90);
  if (line2 && strlen(line2) > 0) {
    tft.setTextSize(1);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(line2, 10, 120);
  }
}

// ---- Station picker web UI ----------------------------------------------

String buildPickerPage() {
  String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#111;color:#eee;padding:16px}"
    "a{display:block;padding:12px;margin-bottom:6px;background:#222;color:#eee;"
    "text-decoration:none;border-radius:6px}a:hover{background:#333}"
    "h2{color:#6CBE45}</style></head><body>"
    "<h2>Pick a G train station</h2>";
  for (int i = 0; i < STATION_COUNT; i++) {
    html += "<a href='/save?stop=" + String(STATIONS[i].stopId) +
            "&name=" + String(STATIONS[i].name) + "'>" +
            String(STATIONS[i].name) + "</a>";
  }
  html += "</body></html>";
  return html;
}

void handlePickerRoot() {
  webServer.send(200, "text/html", buildPickerPage());
}

void handleSave() {
  if (!webServer.hasArg("stop") || !webServer.hasArg("name")) {
    webServer.send(400, "text/plain", "Missing params");
    return;
  }
  savedStopId = webServer.arg("stop");
  savedStopName = webServer.arg("name");

  prefs.begin("subway", false);
  prefs.putString("stop", savedStopId);
  prefs.putString("name", savedStopName);
  prefs.end();

  webServer.send(200, "text/html",
    "<html><body style='font-family:sans-serif;background:#111;color:#eee;padding:16px'>"
    "<h2>Saved: " + savedStopName + "</h2>"
    "<p>The board will show arrivals shortly.</p></body></html>");

  lastFetch = 0;  // force an immediate refresh next loop
}

// ---- Arrivals fetch + draw ------------------------------------------------

void drawArrivals(JsonArray directions) {
  tft.fillScreen(TFT_BLACK);

  tft.fillRect(0, 0, SCREEN_W, HEADER_H, C_G);
  tft.setTextColor(TFT_BLACK, C_G);
  tft.setTextSize(2);
  tft.drawString(savedStopName, 8, 9);

  #define SEC_H  16
  #define ROW_H  36
  #define COL_W  160

  tft.drawFastVLine(COL_W, HEADER_H, SCREEN_H - HEADER_H, 0x2104);

  for (int ci = 0; ci < 2; ci++) {
    int found = -1, count = 0;
    for (int di = 0; di < (int)directions.size(); di++) {
      JsonObject dir = directions[di];
      if (dir["arrivals"].as<JsonArray>().size() > 0) {
        if (count == ci) { found = di; break; }
        count++;
      }
    }
    if (found < 0) continue;

    JsonObject dir = directions[found];
    String dirLabel = dir["label"].as<String>();
    JsonArray arr = dir["arrivals"].as<JsonArray>();
    int x = ci * COL_W;

    tft.fillRect(x, HEADER_H, COL_W, SEC_H, 0x1082);
    tft.setTextColor(0x6B4D, 0x1082);
    tft.setTextSize(1);
    String lbl = dirLabel;
    if (lbl.length() > 20) lbl = lbl.substring(0, 20);
    tft.drawString(lbl.c_str(), x + 4, HEADER_H + 4);

    int y = HEADER_H + SEC_H;
    int rowIdx = 0;
    for (JsonObject a : arr) {
      if (y + ROW_H > SCREEN_H - 8) break;
      String route    = a["route"].as<String>();
      int    mins     = a["mins"].as<int>();
      String terminal = a["terminal"].as<String>();
      if (terminal.length() == 0) terminal = dirLabel;

      uint16_t rowBg = (rowIdx % 2 == 0) ? 0x0841 : TFT_BLACK;
      tft.fillRect(x, y, COL_W, ROW_H, rowBg);

      tft.fillCircle(x + 16, y + 16, 11, C_G);
      tft.setTextColor(TFT_BLACK, C_G);
      tft.setTextSize(2);
      tft.setCursor(x + 16 - (route.length() * 6), y + 9);
      tft.print(route);

      String dest = terminal;
      if (dest.length() > 14) dest = dest.substring(0, 14);
      tft.setTextColor(TFT_LIGHTGREY, rowBg);
      tft.setTextSize(1);
      tft.drawString(dest.c_str(), x + 32, y + 6);

      String timeStr = (mins == 0) ? "Now" : String(mins) + "m";
      tft.setTextColor(TFT_WHITE, rowBg);
      tft.drawString(timeStr.c_str(), x + 32, y + 18);

      y += ROW_H;
      rowIdx++;
    }
  }
}

void fetchAndShowArrivals() {
  if (WiFi.status() != WL_CONNECTED) {
    showMessage("WiFi lost", "Reconnecting...");
    WiFi.reconnect();
    return;
  }
  String url = String(PROXY_BASE) + "?stop=" + savedStopId + "&feed=g";
  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code > 0) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);
    drawArrivals(doc["directions"]);
  } else {
    showMessage("Fetch error", ("HTTP " + String(code)).c_str());
  }
  http.end();
  lastFetch = millis();
}

// ---- Setup / loop ---------------------------------------------------------

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
  checkForOTAUpdate(BOARD_ASSET_NAME, showMessage);  // reboots device in-place if an update is applied

  prefs.begin("subway", true);
  savedStopId = prefs.getString("stop", "");
  savedStopName = prefs.getString("name", "");
  prefs.end();

  webServer.on("/", handlePickerRoot);
  webServer.on("/save", handleSave);
  webServer.begin();

  if (savedStopId.length() > 0) {
    lastFetch = 0;
    fetchAndShowArrivals();
  } else {
    showMessage("No station set", "Visit http://" MDNS_HOSTNAME ".local");
  }
}

void loop() {
  webServer.handleClient();

  if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
    lastOtaCheck = millis();
    checkForOTAUpdate(BOARD_ASSET_NAME, showMessage);
  }

  if (savedStopId.length() > 0 && millis() - lastFetch >= FETCH_INTERVAL) {
    fetchAndShowArrivals();
  }
}
