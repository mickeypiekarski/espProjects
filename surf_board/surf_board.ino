// surf_board.ino
//
// Standalone surf-conditions board. WiFiManager setup -> OTA check (scoped
// to this board's own release asset) -> location search (Open-Meteo
// Geocoding API, on-device, not a hardcoded list) -> live wave/wind
// conditions (Open-Meteo Marine + Weather APIs), cycling between two
// screens every ~6s. Data refreshes every 10 minutes.
//
// This is a complete, independent binary — not a mode inside a larger
// picker. Boards become "a surf board" by being flashed with THIS file via
// the installer (see espProjects/installer/), not by picking a mode at
// runtime. See shared/ota_update/board_template.ino.example for the scaffold
// this was built from, and shared/ota_update/README.md for how to cut a
// release with this board's .bin attached.
//
// Location picking is search-based, not a fixed list: this board is sold
// nationwide, so a hardcoded spot table (like subway_board's 12-station
// G-line array) can't scale. The board itself calls Open-Meteo's geocoding
// API at setup time; only the resulting lat/lon is stored afterward.

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

// ---- Location search web UI ---------------------------------------------

String buildSearchPage() {
  return "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#111;color:#eee;padding:16px}"
    "input[type=text]{width:100%;padding:11px;font-size:1rem;border-radius:8px;"
    "border:1px solid #444;background:#222;color:#eee;margin-bottom:10px}"
    "button{padding:11px 20px;border-radius:8px;border:none;background:#04f;"
    "color:#fff;font-size:1rem}"
    "h2{color:#4ad}</style></head><body>"
    "<h2>Find your surf spot</h2>"
    "<form method='get' action='/search'>"
    "<input type='text' name='q' placeholder='e.g. Rockaway Beach' autofocus>"
    "<button type='submit'>Search</button></form></body></html>";
}

void handlePickerRoot() {
  webServer.send(200, "text/html", buildSearchPage());
}

String urlEncode(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else if (c == ' ') {
      out += '+';
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      out += buf;
    }
  }
  return out;
}

// WebServer::arg() returns raw query-string values — it does not decode
// percent-escapes or '+' back to spaces, so anything encoded via
// urlEncode() above must be decoded again on the way back in.
String urlDecode(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') {
      out += ' ';
    } else if (c == '%' && i + 2 < s.length()) {
      char hex[3] = { s[i + 1], s[i + 2], 0 };
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else {
      out += c;
    }
  }
  return out;
}

String pageHead(const String& title) {
  return "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#111;color:#eee;padding:16px}"
    "a{display:block;padding:12px;margin-bottom:6px;background:#222;color:#eee;"
    "text-decoration:none;border-radius:6px}a:hover{background:#333}"
    "h2{color:#4ad}.sub{color:#888;font-size:0.85em}</style></head><body>"
    "<h2>" + title + "</h2>";
}

// Step 1: area/city search (Open-Meteo Geocoding). Links each result to
// /beaches instead of /save directly, since a city name alone is not
// precise enough for a coastline — it's a population centroid, which can be
// a mile or more from the actual water.
void handleSearch() {
  if (!webServer.hasArg("q") || webServer.arg("q").length() == 0) {
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
    return;
  }
  String query = webServer.arg("q");

  String url = "https://geocoding-api.open-meteo.com/v1/search?name=" + urlEncode(query) +
               "&count=5&language=en&format=json";

  JsonDocument doc;
  bool ok = fetchOpenMeteo(url, doc);

  String page = pageHead("Results for \"" + query + "\"");

  if (!ok) {
    page += "<p>Search failed — check WiFi and try again.</p>";
  } else {
    JsonArray results = doc["results"].as<JsonArray>();
    if (results.size() == 0) {
      page += "<p>No matches for \"" + query + "\" — try a different search.</p>";
    } else {
      for (JsonObject r : results) {
        String name = r["name"] | "";
        String admin1 = r["admin1"] | "";
        String country = r["country"] | "";
        float lat = r["latitude"] | 0.0f;
        float lon = r["longitude"] | 0.0f;

        String area = name;
        if (admin1.length() > 0) area += ", " + admin1;

        String sub = admin1.length() > 0 ? admin1 : country;
        if (country.length() > 0 && admin1.length() > 0) sub += ", " + country;
        else if (country.length() > 0) sub = country;

        char latStr[16], lonStr[16];
        snprintf(latStr, sizeof(latStr), "%.4f", lat);
        snprintf(lonStr, sizeof(lonStr), "%.4f", lon);

        page += "<a href='/beaches?lat=" + String(latStr) + "&amp;lon=" + String(lonStr) +
                "&amp;area=" + urlEncode(area) + "'>" + name +
                "<br><span class='sub'>" + sub + "</span></a>";
      }
    }
  }
  page += "</body></html>";

  webServer.send(200, "text/html", page);
}

// Overpass QL body: named beach nodes within a small bbox around (lat, lon).
// Overpass only performs well bounded to a small area — a nationwide name
// search on this API takes 40s+ and isn't usable interactively, which is
// exactly why this step only runs after step 1 has already narrowed things
// down to a specific city/area.
String buildOverpassQuery(float lat, float lon) {
  char q[256];
  const float DELTA = 0.15f;  // ~10-15mi depending on latitude
  snprintf(q, sizeof(q),
    "[out:json][timeout:15];node[\"natural\"=\"beach\"][\"name\"](%.4f,%.4f,%.4f,%.4f);out body;",
    lat - DELTA, lon - DELTA, lat + DELTA, lon + DELTA);
  return String(q);
}

bool fetchOverpassBeachesOnce(float lat, float lon, JsonDocument& doc) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, "https://overpass-api.de/api/interpreter")) return false;
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "data=" + urlEncode(buildOverpassQuery(lat, lon));
  int code = http.POST(body);
  bool ok = false;
  if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    ok = (deserializeJson(doc, payload) == DeserializationError::Ok);
  }
  http.end();
  return ok;
}

// Overpass's free public instance occasionally 504s under load on an
// otherwise-valid request (confirmed live: identical request failed, then
// succeeded seconds later) — one retry meaningfully reduces how often a
// real nearby beach gets missed due to a momentary server hiccup.
bool fetchOverpassBeaches(float lat, float lon, JsonDocument& doc) {
  if (fetchOverpassBeachesOnce(lat, lon, doc)) return true;
  delay(1000);
  return fetchOverpassBeachesOnce(lat, lon, doc);
}

// Step 2: look up named beaches near the step-1 area point. No match (or a
// failed/slow Overpass request) falls back to saving the area point as-is —
// deliberately non-blocking, so a real but less-mapped stretch of coast
// never prevents finishing setup.
void handleBeaches() {
  if (!webServer.hasArg("lat") || !webServer.hasArg("lon") || !webServer.hasArg("area")) {
    webServer.send(400, "text/plain", "Missing params");
    return;
  }
  float lat = webServer.arg("lat").toFloat();
  float lon = webServer.arg("lon").toFloat();
  String area = urlDecode(webServer.arg("area"));

  char latStr[16], lonStr[16];
  snprintf(latStr, sizeof(latStr), "%.4f", lat);
  snprintf(lonStr, sizeof(lonStr), "%.4f", lon);

  JsonDocument doc;
  bool ok = fetchOverpassBeaches(lat, lon, doc);
  JsonArray elements = ok ? doc["elements"].as<JsonArray>() : JsonArray();

  if (!ok || elements.size() == 0) {
    String page = pageHead(ok ? "No named beach found nearby" : "Beach lookup unavailable");
    if (!ok) page += "<p>The beach lookup service didn't respond — this can happen occasionally.</p>";
    page += "<p>Using " + area + " instead.</p>";
    page += "<a href='/save?lat=" + String(latStr) + "&amp;lon=" + String(lonStr) +
            "&amp;label=" + urlEncode(area) + "'>Continue with " + area + "</a>";
    webServer.send(200, "text/html", page);
    return;
  }

  String page = pageHead("Beaches near " + area);
  for (JsonObject el : elements) {
    String beachName = el["tags"]["name"] | "";
    if (beachName.length() == 0) continue;
    float blat = el["lat"] | lat;
    float blon = el["lon"] | lon;

    char blatStr[16], blonStr[16];
    snprintf(blatStr, sizeof(blatStr), "%.4f", blat);
    snprintf(blonStr, sizeof(blonStr), "%.4f", blon);

    String label = beachName + ", " + area;

    page += "<a href='/save?lat=" + String(blatStr) + "&amp;lon=" + String(blonStr) +
            "&amp;label=" + urlEncode(label) + "'>" + beachName + "</a>";
  }
  page += "</body></html>";

  webServer.send(200, "text/html", page);
}

void handleSave() {
  if (!webServer.hasArg("lat") || !webServer.hasArg("lon") || !webServer.hasArg("label")) {
    webServer.send(400, "text/plain", "Missing params");
    return;
  }
  savedLat = webServer.arg("lat").toFloat();
  savedLon = webServer.arg("lon").toFloat();
  savedName = urlDecode(webServer.arg("label"));

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
  webServer.on("/search", handleSearch);
  webServer.on("/beaches", handleBeaches);
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
