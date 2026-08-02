// surf_board.ino
//
// Standalone surf-conditions board. WiFiManager setup -> OTA check (scoped
// to this board's own release asset) -> pick a spot from a baked-in list of
// named US surf breaks -> live wave/wind conditions (Open-Meteo Marine +
// Weather APIs), cycling between two screens every ~6s. Data refreshes
// every 10 minutes.
//
// This is a complete, independent binary — not a mode inside a larger
// picker. Boards become "a surf board" by being flashed with THIS file via
// the installer (see espProjects/installer/), not by picking a mode at
// runtime. See shared/ota_update/board_template.ino.example for the scaffold
// this was built from, and shared/ota_update/README.md for how to cut a
// release with this board's .bin attached.
//
// Location picking is a baked-in spot table (SURF_SPOTS below), same
// pattern as subway_board's station list — NOT live search. An earlier
// version of this board chained Open-Meteo geocoding + OpenStreetMap
// Overpass lookups to let users search any US location; that was dropped
// because Overpass's free public instance is unreliable (confirmed live:
// intermittent 504s on valid requests) and OSM's beach tagging doesn't
// reliably cover well-known named breaks (e.g. Venice Beach). A curated
// list has zero runtime dependency on any third-party lookup, so it can't
// fail the way that live chain did — the tradeoff is v1 only covers the
// ~80 spots below, not "any US coastline."

#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

#define BOARD_NAME "SurfBoard"
#define MDNS_HOSTNAME "surfboard"          // reachable at http://surfboard.local
#define FW_VERSION_CODE 10000              // v1.0.0 — bump on every release
#define BOARD_ASSET_NAME "surf_board.bin"

#include "ota_update.h"

TFT_eSPI tft = TFT_eSPI();
WiFiManager wifiManager;
WebServer webServer(80);
Preferences prefs;

unsigned long lastOtaCheck = 0;

#define SCREEN_W 320
#define SCREEN_H 240
#define HEADER_H 34
#define FETCH_INTERVAL (10UL * 60 * 1000)   // 10 minutes — marine/weather models update roughly hourly
#define CYCLE_INTERVAL 6000                  // 6 seconds between screens

#define C_HEADER 0x04FF  // ocean-blue header bar

float savedLat = NAN;
float savedLon = NAN;
String savedName = "";

unsigned long lastFetch = 0;
unsigned long lastCycle = 0;
int currentScreen = 0;  // 0 = waves, 1 = wind

struct SurfConditions {
  bool valid = false;
  float waveHeight = 0, wavePeriod = 0, waveDirection = 0;
  float swellHeight = 0, swellPeriod = 0;
  float windSpeed = 0, windDirection = 0, windGust = 0, tempC = 0;
};
SurfConditions conditions;

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

// Buckets degrees into a 16-point compass abbreviation (N, NNE, NE, ...).
const char* compassDir(float degrees) {
  static const char* DIRS[16] = {
    "N","NNE","NE","ENE","E","ESE","SE","SSE",
    "S","SSW","SW","WSW","W","WNW","NW","NNW"
  };
  int idx = (int)((degrees / 22.5f) + 0.5f) % 16;
  if (idx < 0) idx += 16;
  return DIRS[idx];
}

// ---- HTTPS fetch helper (explicit WiFiClientSecure, matches ota_update.h's
// hardware-proven pattern rather than an implicit https:// begin()) --------

bool fetchOpenMeteo(const String& url, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    ok = (deserializeJson(doc, payload) == DeserializationError::Ok);
  }
  http.end();
  return ok;
}

// ---- Surf spot database (baked in, not live-searched) --------------------
// Real named US surf breaks with coordinates at/near the actual break, not
// city centroids. Add more spots here and bump FW_VERSION_CODE to ship an
// update — same workflow as adding a subway_board station.

struct SurfSpot { const char* name; float lat; float lon; };

const SurfSpot SURF_SPOTS[] = {
  // California
  {"Malibu (First Point), CA", 34.0359, -118.6774},
  {"Venice Beach, CA", 33.9850, -118.4695},
  {"Manhattan Beach, CA", 33.8847, -118.4109},
  {"Huntington Beach Pier, CA", 33.6553, -118.0053},
  {"Trestles, CA", 33.3825, -117.5936},
  {"Rincon, CA", 34.3708, -119.4772},
  {"Santa Cruz - Steamer Lane, CA", 36.9513, -122.0247},
  {"Mavericks, CA", 37.4947, -122.5008},
  {"Ocean Beach, San Francisco, CA", 37.7600, -122.5107},
  {"Pacifica - Linda Mar, CA", 37.5985, -122.5010},
  {"Pismo Beach, CA", 35.1428, -120.6413},
  {"San Onofre, CA", 33.3719, -117.5678},
  {"Newport Beach, CA", 33.6057, -117.9298},
  {"La Jolla Shores, CA", 32.8572, -117.2570},
  {"Swami's, Encinitas, CA", 33.0300, -117.2914},
  {"Windansea, La Jolla, CA", 32.8207, -117.2782},
  {"Blacks Beach, La Jolla, CA", 32.8836, -117.2530},
  {"Oceanside Pier, CA", 33.1959, -117.3795},
  {"Sunset Cliffs, San Diego, CA", 32.7157, -117.2544},

  // Hawaii
  {"Pipeline, Oahu, HI", 21.6650, -158.0533},
  {"Sunset Beach, Oahu, HI", 21.6753, -158.0397},
  {"Waimea Bay, Oahu, HI", 21.6392, -158.0669},
  {"Waikiki, Oahu, HI", 21.2761, -157.8278},
  {"Honolua Bay, Maui, HI", 21.0089, -156.6425},
  {"Hookipa Beach, Maui, HI", 20.9333, -156.3583},
  {"Poipu Beach, Kauai, HI", 21.8721, -159.4573},

  // New York / New Jersey
  {"Rockaway Beach, Queens, NY", 40.5820, -73.8154},
  {"Long Beach, NY", 40.5834, -73.6660},
  {"Montauk, NY", 41.0362, -71.9509},
  {"Lido Beach, NY", 40.5868, -73.6168},
  {"Manasquan, NJ", 40.1043, -74.0332},
  {"Belmar, NJ", 40.1793, -74.0121},
  {"Long Beach Island, NJ", 39.6543, -74.1801},

  // New England
  {"Narragansett, RI", 41.4356, -71.4506},
  {"Cape Cod - Nauset Beach, MA", 41.7590, -69.9578},
  {"Wells Beach, ME", 43.3226, -70.5850},

  // Florida / Gulf
  {"Cocoa Beach, FL", 28.3200, -80.6076},
  {"New Smyrna Beach, FL", 29.0258, -80.9270},
  {"Sebastian Inlet, FL", 27.8597, -80.4478},
  {"South Beach, Miami, FL", 25.7826, -80.1300},
  {"Folly Beach, SC", 32.6551, -79.9403},
  {"Wrightsville Beach, NC", 34.2085, -77.7967},
  {"Outer Banks - Nags Head, NC", 35.9573, -75.6241},
  {"Virginia Beach, VA", 36.8529, -75.9780},
  {"Galveston, TX", 29.2477, -94.8536},
  {"South Padre Island, TX", 26.0989, -97.1653},

  // Pacific Northwest
  {"Westport, WA", 46.8845, -124.1087},
  {"Cannon Beach, OR", 45.8918, -123.9615},
  {"Seaside, OR", 45.9932, -123.9226},
  {"Pacific City, OR", 45.2032, -123.9576},
};
const int SURF_SPOT_COUNT = sizeof(SURF_SPOTS) / sizeof(SURF_SPOTS[0]);

// ---- Spot picker web UI ---------------------------------------------------

String buildPickerPage() {
  String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#111;color:#eee;padding:16px}"
    "input{width:100%;padding:11px;font-size:1rem;border-radius:8px;border:1px solid #444;"
    "background:#222;color:#eee;margin-bottom:10px}"
    "a{display:block;padding:10px;margin-bottom:4px;background:#222;color:#eee;"
    "text-decoration:none;border-radius:6px}a:hover{background:#333}"
    "h2{color:#4ad}</style></head><body>"
    "<h2>Pick your spot</h2>"
    "<input type='text' id='f' placeholder='Filter...' oninput='"
    "document.querySelectorAll(\"#list a\").forEach(a=>a.style.display="
    "a.textContent.toLowerCase().includes(this.value.toLowerCase())?\"\":\"none\")'>"
    "<div id='list'>";
  for (int i = 0; i < SURF_SPOT_COUNT; i++) {
    html += "<a href='/save?i=" + String(i) + "'>" + String(SURF_SPOTS[i].name) + "</a>";
  }
  html += "</div></body></html>";
  return html;
}

void handlePickerRoot() {
  webServer.send(200, "text/html", buildPickerPage());
}

void handleSave() {
  if (!webServer.hasArg("i")) {
    webServer.send(400, "text/plain", "Missing param");
    return;
  }
  int i = webServer.arg("i").toInt();
  if (i < 0 || i >= SURF_SPOT_COUNT) {
    webServer.send(400, "text/plain", "Invalid spot");
    return;
  }

  savedLat = SURF_SPOTS[i].lat;
  savedLon = SURF_SPOTS[i].lon;
  savedName = SURF_SPOTS[i].name;

  prefs.begin("surf", false);
  prefs.putFloat("lat", savedLat);
  prefs.putFloat("lon", savedLon);
  prefs.putString("name", savedName);
  prefs.end();

  webServer.send(200, "text/html",
    "<html><body style='font-family:sans-serif;background:#111;color:#eee;padding:16px'>"
    "<h2>Saved: " + savedName + "</h2>"
    "<p>The board will show conditions shortly.</p></body></html>");

  lastFetch = 0;  // force an immediate refresh next loop
}

// ---- Drawing --------------------------------------------------------------

void drawHeader() {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, C_HEADER);
  tft.setTextColor(TFT_WHITE, C_HEADER);
  tft.setTextSize(2);
  String name = savedName;
  if (name.length() > 26) name = name.substring(0, 26);
  tft.drawString(name, 8, 9);

  // Screen indicator dots (top-right corner)
  int dotX = SCREEN_W - 24;
  for (int i = 0; i < 2; i++) {
    uint16_t color = (i == currentScreen) ? TFT_WHITE : 0x2104;
    tft.fillCircle(dotX + i * 14, HEADER_H / 2, 4, color);
  }
}

void drawRow(int y, const char* label, const String& value) {
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString(label, 12, y);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(3);
  tft.drawString(value, 12, y + 12);
}

void drawWavesScreen() {
  tft.fillScreen(TFT_BLACK);
  drawHeader();

  char buf[24];
  int y = HEADER_H + 14;
  #define ROW_STEP 38

  snprintf(buf, sizeof(buf), "%.1f m", conditions.waveHeight);
  drawRow(y, "WAVE HEIGHT", buf);
  y += ROW_STEP;

  snprintf(buf, sizeof(buf), "%.1f s", conditions.wavePeriod);
  drawRow(y, "WAVE PERIOD", buf);
  y += ROW_STEP;

  snprintf(buf, sizeof(buf), "%.1f m", conditions.swellHeight);
  drawRow(y, "SWELL HEIGHT", buf);
  y += ROW_STEP;

  snprintf(buf, sizeof(buf), "%.1f s  %s", conditions.swellPeriod, compassDir(conditions.waveDirection));
  drawRow(y, "SWELL PERIOD / DIR", buf);
}

void drawWindScreen() {
  tft.fillScreen(TFT_BLACK);
  drawHeader();

  char buf[24];
  int y = HEADER_H + 14;

  snprintf(buf, sizeof(buf), "%.0f km/h", conditions.windSpeed);
  drawRow(y, "WIND SPEED", buf);
  y += ROW_STEP;

  snprintf(buf, sizeof(buf), "%.0f km/h", conditions.windGust);
  drawRow(y, "GUSTS", buf);
  y += ROW_STEP;

  snprintf(buf, sizeof(buf), "%s", compassDir(conditions.windDirection));
  drawRow(y, "DIRECTION", buf);
  y += ROW_STEP;

  snprintf(buf, sizeof(buf), "%.0f C", conditions.tempC);
  drawRow(y, "AIR TEMP", buf);
}

void drawCurrentScreen() {
  if (currentScreen == 0) drawWavesScreen();
  else drawWindScreen();
}

// ---- Conditions fetch -------------------------------------------------

void fetchConditions() {
  if (WiFi.status() != WL_CONNECTED) {
    showMessage("WiFi lost", "Reconnecting...");
    WiFi.reconnect();
    lastFetch = millis();
    return;
  }

  char marineUrl[192], weatherUrl[192];
  snprintf(marineUrl, sizeof(marineUrl),
    "https://marine-api.open-meteo.com/v1/marine?latitude=%.4f&longitude=%.4f"
    "&current=wave_height,wave_direction,wave_period,swell_wave_height,swell_wave_period&timezone=auto",
    savedLat, savedLon);
  snprintf(weatherUrl, sizeof(weatherUrl),
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=wind_speed_10m,wind_direction_10m,wind_gusts_10m,temperature_2m&timezone=auto",
    savedLat, savedLon);

  bool marineOk, weatherOk;
  {
    JsonDocument doc;
    marineOk = fetchOpenMeteo(marineUrl, doc);
    if (marineOk) {
      JsonObject cur = doc["current"];
      conditions.waveHeight    = cur["wave_height"]        | 0.0f;
      conditions.waveDirection = cur["wave_direction"]     | 0.0f;
      conditions.wavePeriod    = cur["wave_period"]         | 0.0f;
      conditions.swellHeight   = cur["swell_wave_height"]  | 0.0f;
      conditions.swellPeriod   = cur["swell_wave_period"]  | 0.0f;
    }
  }
  {
    JsonDocument doc;
    weatherOk = fetchOpenMeteo(weatherUrl, doc);
    if (weatherOk) {
      JsonObject cur = doc["current"];
      conditions.windSpeed     = cur["wind_speed_10m"]     | 0.0f;
      conditions.windDirection = cur["wind_direction_10m"] | 0.0f;
      conditions.windGust      = cur["wind_gusts_10m"]     | 0.0f;
      conditions.tempC         = cur["temperature_2m"]     | 0.0f;
    }
  }

  conditions.valid = marineOk || weatherOk;
  if (!conditions.valid) {
    showMessage("Fetch error", "Check connection");
  } else {
    drawCurrentScreen();
  }
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

  prefs.begin("surf", true);
  savedLat = prefs.getFloat("lat", NAN);
  savedLon = prefs.getFloat("lon", NAN);
  savedName = prefs.getString("name", "");
  prefs.end();

  webServer.on("/", handlePickerRoot);
  webServer.on("/save", handleSave);
  webServer.begin();

  if (!isnan(savedLat)) {
    lastFetch = 0;
    fetchConditions();
  } else {
    showMessage("No location set", "Visit http://" MDNS_HOSTNAME ".local");
  }
}

void loop() {
  webServer.handleClient();

  if (millis() - lastOtaCheck >= OTA_CHECK_INTERVAL_MS) {
    lastOtaCheck = millis();
    checkForOTAUpdate(BOARD_ASSET_NAME, showMessage);
  }

  if (!isnan(savedLat) && millis() - lastFetch >= FETCH_INTERVAL) {
    fetchConditions();
  }

  if (conditions.valid && millis() - lastCycle >= CYCLE_INTERVAL) {
    lastCycle = millis();
    currentScreen = (currentScreen + 1) % 2;
    drawCurrentScreen();
  }
}
