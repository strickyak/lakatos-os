#include "lakatos_graffiti.h"
#include "lakatos_state.h"
#include "lakatos_version.h"
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <WiFi.h>
#include <bootloader_random.h>
#include <esp32/rom/crc.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <math.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Touch Calibration State (NVS namespace "cyd_cal", shared with demo1/demo2)
// ---------------------------------------------------------------------------
struct TouchCalibration {
  bool  swapXY = false;
  bool  kbGraf = false;
  float uAtX1  = 650.0f;
  float uAtX2  = 3350.0f;
  float vAtY1  = 980.0f;
  float vAtY2  = 3280.0f;
  int   calX1  = 40;
  int   calX2  = 280;
  int   calY1  = 50;
  int   calY2  = 200;
};

static TouchCalibration gCal;

bool loadTouchCalibrationFromNvs() {
  prefs.begin("cyd_cal", true);
  bool valid = prefs.getBool("valid", false);
  if (valid) {
    gCal.swapXY = prefs.getBool("swapXY", false);
    gCal.uAtX1  = prefs.getFloat("u1", 650.0f);
    gCal.uAtX2  = prefs.getFloat("u2", 3350.0f);
    gCal.vAtY1  = prefs.getFloat("v1", 980.0f);
    gCal.vAtY2  = prefs.getFloat("v2", 3280.0f);
  }
  prefs.end();
  return valid;
}

static void saveTouchCalibrationToNvs() {
  prefs.begin("cyd_cal", false);
  prefs.putBool("valid", true);
  prefs.putBool("swapXY", gCal.swapXY);
  prefs.putFloat("u1", gCal.uAtX1);
  prefs.putFloat("u2", gCal.uAtX2);
  prefs.putFloat("v1", gCal.vAtY1);
  prefs.putFloat("v2", gCal.vAtY2);
  prefs.end();
}

void mapRawTouchToScreen(int rawX, int rawY, int& sx, int& sy) {
  float u = gCal.swapXY ? (float)rawY : (float)rawX;
  float v = gCal.swapXY ? (float)rawX : (float)rawY;
  float du = gCal.uAtX2 - gCal.uAtX1;
  float dv = gCal.vAtY2 - gCal.vAtY1;
  if (fabsf(du) < 1.0f) du = 1.0f;
  if (fabsf(dv) < 1.0f) dv = 1.0f;

  float fx = gCal.calX1 + (u - gCal.uAtX1) * (float)(gCal.calX2 - gCal.calX1) / du;
  float fy = gCal.calY1 + (v - gCal.vAtY1) * (float)(gCal.calY2 - gCal.calY1) / dv;
  sx = constrain((int)lroundf(fx), 0, LAKATOS_SCREEN_W - 1);
  sy = constrain((int)lroundf(fy), 0, LAKATOS_SCREEN_H - 1);
}

// ---------------------------------------------------------------------------
// Airport Database (Coordinates + POSIX Timezone)
// ---------------------------------------------------------------------------
struct KnownAirport {
  const char* iata;
  const char* city;
  const char* posixTz;
  float       lat;
  float       lon;
};

static const KnownAirport KNOWN_AIRPORTS[] = {
  { "ATL", "Atlanta, GA",        "EST5EDT,M3.2.0,M11.1.0",   33.6407f,  -84.4277f },
  { "SFO", "San Francisco, CA",  "PST8PDT,M3.2.0,M11.1.0",   37.6213f, -122.3790f },
  { "NYC", "New York (JFK), NY", "EST5EDT,M3.2.0,M11.1.0",   40.6413f,  -73.7781f },
  { "JFK", "New York (JFK), NY", "EST5EDT,M3.2.0,M11.1.0",   40.6413f,  -73.7781f },
  { "LAX", "Los Angeles, CA",    "PST8PDT,M3.2.0,M11.1.0",   33.9416f, -118.4085f },
  { "ORD", "Chicago O'Hare, IL", "CST6CDT,M3.2.0,M11.1.0",   41.9742f,  -87.9073f },
  { "DEN", "Denver, CO",         "MST7MDT,M3.2.0,M11.1.0",   39.8561f, -104.6737f },
  { "SEA", "Seattle-Tacoma, WA", "PST8PDT,M3.2.0,M11.1.0",   47.4502f, -122.3088f },
  { "BOS", "Boston, MA",         "EST5EDT,M3.2.0,M11.1.0",   42.3656f,  -71.0096f },
  { "DFW", "Dallas/Ft Worth, TX","CST6CDT,M3.2.0,M11.1.0",   32.8998f,  -97.0403f },
  { "MIA", "Miami, FL",          "EST5EDT,M3.2.0,M11.1.0",   25.7959f,  -80.2870f },
  { "IAH", "Houston, TX",        "CST6CDT,M3.2.0,M11.1.0",   29.9902f,  -95.3368f },
  { "PHX", "Phoenix, AZ",        "MST7",                     33.4352f, -112.0101f },
  { "LAS", "Las Vegas, NV",      "PST8PDT,M3.2.0,M11.1.0",   36.0840f, -115.1537f },
  { "MSP", "Minneapolis, MN",    "CST6CDT,M3.2.0,M11.1.0",   44.8848f,  -93.2223f },
  { "PDX", "Portland, OR",       "PST8PDT,M3.2.0,M11.1.0",   45.5898f, -122.5951f },
  { "HNL", "Honolulu, HI",       "HST10",                    21.3187f, -157.9225f },
  { "ANC", "Anchorage, AK",      "AKST9AKDT,M3.2.0,M11.1.0", 61.1743f, -149.9962f },
  { "DCA", "Washington, DC",     "EST5EDT,M3.2.0,M11.1.0",   38.8512f,  -77.0402f },
  { "IAD", "Dulles, VA",         "EST5EDT,M3.2.0,M11.1.0",   38.9531f,  -77.4565f },
  { "PHL", "Philadelphia, PA",   "EST5EDT,M3.2.0,M11.1.0",   39.8744f,  -75.2424f },
  { "CLT", "Charlotte, NC",      "EST5EDT,M3.2.0,M11.1.0",   35.2144f,  -80.9473f },
  { "MCO", "Orlando, FL",        "EST5EDT,M3.2.0,M11.1.0",   28.4312f,  -81.3081f },
  { "DTW", "Detroit, MI",        "EST5EDT,M3.2.0,M11.1.0",   42.2162f,  -83.3554f },
  { "AUS", "Austin, TX",         "CST6CDT,M3.2.0,M11.1.0",   30.1975f,  -97.6664f },
  { "SLC", "Salt Lake City, UT", "MST7MDT,M3.2.0,M11.1.0",   40.7899f, -111.9791f },
  { "SAN", "San Diego, CA",      "PST8PDT,M3.2.0,M11.1.0",   32.7338f, -117.1933f },
  { "SJC", "San Jose, CA",       "PST8PDT,M3.2.0,M11.1.0",   37.3639f, -121.9289f },
  { "OAK", "Oakland, CA",        "PST8PDT,M3.2.0,M11.1.0",   37.7126f, -122.2197f },
};
static const int NUM_KNOWN_AIRPORTS = sizeof(KNOWN_AIRPORTS) / sizeof(KNOWN_AIRPORTS[0]);

static const char* const QUICK_AIRPORTS[12] = {
  "ATL", "SFO", "NYC", "LAX",
  "ORD", "DEN", "SEA", "BOS",
  "DFW", "MIA", "PHX", "HNL"
};

// ---------------------------------------------------------------------------
// Persistent OS Configuration State (NVS "cyd_cfg" & "cyd_wifi")
// ---------------------------------------------------------------------------
static bool   gAutoBright      = true;
static int    gManualBrightPct = 85;
static int    gCurrentBrightPct = 85;
static float  gSmoothedLdr     = 1500.0f;
static uint32_t gLastLdrMs     = 0;
static int    gLastPwm         = -1;

static char   gAirportCode[5]  = "SFO";
static char   gAirportCity[24] = "San Francisco, CA";
static char   gAirportTz[32]   = "PST8PDT,M3.2.0,M11.1.0";
static float  gAirportLat      = 37.6213f;
static float  gAirportLon      = -122.3790f;
static bool   gNtpStarted      = false;

static char   gDeviceName[32]  = "test";
static char   gSerialNo[32]    = "";
static char   gShUrl[96]       = "http://pizga.net:1908/";
static char   gShPass[64]      = "";  // Never stored in source code; loaded from NVS
static bool   gTouchSpotEnabled = false;
static int    gCornerSize      = 24;

static int    gActivityMode    = LAKATOS_MODE_ACTIVE;
static char   gAmbientApp[32]  = "ambient";
static int    gAmbientSec      = 60;
static int    gDemoSec         = 60;
static int    gActiveIdleSec   = 120;

static void applyAirportCode(const char* code) {
  char up[4] = {'S', 'F', 'O', '\0'};
  for (int i = 0; i < 3 && code && code[i]; i++) {
    char c = code[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    up[i] = c;
  }
  strncpy(gAirportCode, up, sizeof(gAirportCode) - 1);
  gAirportCode[3] = '\0';

  for (int i = 0; i < NUM_KNOWN_AIRPORTS; i++) {
    if (strcmp(KNOWN_AIRPORTS[i].iata, gAirportCode) == 0) {
      strncpy(gAirportCity, KNOWN_AIRPORTS[i].city, sizeof(gAirportCity) - 1);
      gAirportCity[sizeof(gAirportCity) - 1] = '\0';
      strncpy(gAirportTz, KNOWN_AIRPORTS[i].posixTz, sizeof(gAirportTz) - 1);
      gAirportTz[sizeof(gAirportTz) - 1] = '\0';
      gAirportLat = KNOWN_AIRPORTS[i].lat;
      gAirportLon = KNOWN_AIRPORTS[i].lon;
      setenv("TZ", gAirportTz, 1);
      tzset();
      return;
    }
  }
  snprintf(gAirportCity, sizeof(gAirportCity), "%s Airport", gAirportCode);
  setenv("TZ", gAirportTz, 1);
  tzset();
}

static void applyBacklightPercent(int pct) {
  pct = constrain(pct, 5, 100);
  gCurrentBrightPct = pct;
  float norm = (float)pct / 100.0f;
  int pwm = (int)lroundf(12.0f + 243.0f * powf(norm, 1.8f));
  pwm = constrain(pwm, 12, 255);
  if (pwm != gLastPwm) {
    gLastPwm = pwm;
    analogWrite(TFT_BL_PIN, pwm);
  }
}

static int computeAutoBrightPct(float ldr) {
  float clamped = constrain(ldr, 200.0f, 3500.0f);
  float t = (clamped - 200.0f) / (3500.0f - 200.0f);
  int pct = (int)lroundf(100.0f - t * 88.0f);
  return constrain(pct, 12, 100);
}

static void saveConfigToNvs() {
  prefs.begin("cyd_cfg", false);
  prefs.putBool("autoB", gAutoBright);
  prefs.putInt("manB", gManualBrightPct);
  prefs.putInt("cornerSz", gCornerSize);
  prefs.putString("iata", gAirportCode);
  prefs.putString("devName", gDeviceName);
  prefs.putString("serialNo", gSerialNo);
  prefs.putString("shUrl", gShUrl);
  prefs.putString("shPass", gShPass);
  prefs.putString("ambApp2", gAmbientApp);
  prefs.putInt("ambSec", gAmbientSec);
  prefs.putInt("demoSec", gDemoSec);
  prefs.putInt("idleSec2", gActiveIdleSec);
  prefs.putBool("kbGraf", gCal.kbGraf);
  prefs.end();
}

static int snapCornerSize(int sz) {
  static const int kStops[5] = {8, 12, 16, 20, 24};
  int best = kStops[0];
  int bestDist = abs(sz - best);
  for (int i = 1; i < 5; i++) {
    int d = abs(sz - kStops[i]);
    if (d < bestDist) {
      bestDist = d;
      best = kStops[i];
    }
  }
  return best;
}

void initLakatosConfig() {
  pinMode(LDR_PIN, INPUT);
  pinMode(TFT_BL_PIN, OUTPUT);

  prefs.begin("cyd_cfg", true);
  gAutoBright = prefs.getBool("autoB", true);
  gManualBrightPct = constrain((int)prefs.getInt("manB", 85), 5, 100);
  gCornerSize = snapCornerSize((int)prefs.getInt("cornerSz", 24));
  String savedIata = prefs.getString("iata", "SFO");
  String savedDev  = prefs.getString("devName", "test");
  String savedSer  = prefs.getString("serialNo", "");
  String savedUrl  = prefs.getString("shUrl", "http://pizga.net:1908/");
  String savedPass = prefs.getString("shPass", "");
  String savedAmb  = prefs.getString("ambApp2", "ambient");
  gAmbientSec      = constrain((int)prefs.getInt("ambSec", 60), 1, 86400);
  gDemoSec         = constrain((int)prefs.getInt("demoSec", 60), 5, 3600);
  gActiveIdleSec   = constrain((int)prefs.getInt("idleSec2", 120), 0, 86400);
  gCal.kbGraf      = prefs.getBool("kbGraf", false);
  prefs.end();
  gTouchSpotEnabled = false;
  gActivityMode     = LAKATOS_MODE_ACTIVE;

  if (savedAmb.length() > 0) {
    strncpy(gAmbientApp, savedAmb.c_str(), sizeof(gAmbientApp) - 1);
    gAmbientApp[sizeof(gAmbientApp) - 1] = '\0';
  } else {
    strcpy(gAmbientApp, "ambient");
  }

  strncpy(gDeviceName, savedDev.c_str(), sizeof(gDeviceName) - 1);
  strncpy(gShUrl, savedUrl.c_str(), sizeof(gShUrl) - 1);
  strncpy(gShPass, savedPass.c_str(), sizeof(gShPass) - 1);
  applyAirportCode(savedIata.c_str());

  if (savedSer.length() == 0 || savedSer == "ing") {
    bootloader_random_enable();
    uint32_t rnd = (esp_random() % 99999999u) + 1u;
    bootloader_random_disable();
    snprintf(gSerialNo, sizeof(gSerialNo), "%08u", (unsigned)rnd);
    saveConfigToNvs();
  } else {
    strncpy(gSerialNo, savedSer.c_str(), sizeof(gSerialNo) - 1);
  }

  int raw = analogRead(LDR_PIN);
  gSmoothedLdr = (float)raw;
  if (gAutoBright) {
    applyBacklightPercent(computeAutoBrightPct(gSmoothedLdr));
  } else {
    applyBacklightPercent(gManualBrightPct);
  }

  // Auto-connect Wi-Fi in background if credentials saved in "cyd_wifi"
  prefs.begin("cyd_wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    if (pass.length() > 0) {
      WiFi.begin(ssid.c_str(), pass.c_str());
    } else {
      WiFi.begin(ssid.c_str());
    }
    Serial.printf("[Lakatos] Auto-connecting Wi-Fi to '%s'...\n", ssid.c_str());
  }
}

void updateLakatosConfigBackground() {
  uint32_t now = millis();
  if (now - gLastLdrMs >= 100) {
    gLastLdrMs = now;
    int raw = analogRead(LDR_PIN);
    gSmoothedLdr = 0.90f * gSmoothedLdr + 0.10f * (float)raw;
    if (gAutoBright) {
      applyBacklightPercent(computeAutoBrightPct(gSmoothedLdr));
    }
  }

  if (WiFi.status() == WL_CONNECTED && !gNtpStarted) {
    gNtpStarted = true;
    configTzTime(gAirportTz, "pool.ntp.org", "time.nist.gov");
  }
  lakatosPollSerialFuse();
}

const char* lakatosGetAirportCode() { return gAirportCode; }

bool lakatosGetAirportInfo(LakatosAirportInfo* out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  strncpy(out->code, gAirportCode, sizeof(out->code) - 1);
  strncpy(out->city, gAirportCity, sizeof(out->city) - 1);
  strncpy(out->tz, gAirportTz, sizeof(out->tz) - 1);
  out->lat = gAirportLat;
  out->lon = gAirportLon;
  return true;
}

const char* lakatosGetDeviceName() { return gDeviceName; }
const char* lakatosGetSerialNumber() { return gSerialNo; }
const char* lakatosGetSuperhighwayUrl() { return gShUrl; }
const char* lakatosGetSuperhighwayPassword() { return gShPass; }

static bool gCanBeDeveloper = false;
static bool gConfigNeedsRedraw = false;

bool lakatosConsumeConfigRedraw() {
  if (!gConfigNeedsRedraw) return false;
  gConfigNeedsRedraw = false;
  return true;
}

void lakatosSetDeviceName(const char* name) {
  strncpy(gDeviceName, name ? name : "", sizeof(gDeviceName) - 1);
  gDeviceName[sizeof(gDeviceName) - 1] = '\0';
  saveConfigToNvs();
  gConfigNeedsRedraw = true;
}

void lakatosSetSerialNumber(const char* sn) {
  strncpy(gSerialNo, sn ? sn : "", sizeof(gSerialNo) - 1);
  gSerialNo[sizeof(gSerialNo) - 1] = '\0';
  saveConfigToNvs();
  gConfigNeedsRedraw = true;
}

void lakatosSetSuperhighwayUrl(const char* url) {
  strncpy(gShUrl, url ? url : "", sizeof(gShUrl) - 1);
  gShUrl[sizeof(gShUrl) - 1] = '\0';
  saveConfigToNvs();
  gConfigNeedsRedraw = true;
}

void lakatosSetSuperhighwayPassword(const char* pass) {
  strncpy(gShPass, pass ? pass : "", sizeof(gShPass) - 1);
  gShPass[sizeof(gShPass) - 1] = '\0';
  saveConfigToNvs();
  gConfigNeedsRedraw = true;
}

void lakatosSetAirportCode(const char* code) {
  applyAirportCode(code);
  saveConfigToNvs();
  gConfigNeedsRedraw = true;
}

void lakatosSetWifiCredentials(const char* ssid, const char* pass) {
  prefs.begin("cyd_wifi", false);
  prefs.putString("ssid", ssid ? ssid : "");
  prefs.putString("pass", pass ? pass : "");
  prefs.end();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  delay(80);
  WiFi.setSleep(false);
  if (pass && pass[0]) {
    WiFi.begin(ssid, pass);
  } else {
    WiFi.begin(ssid);
  }
  gConfigNeedsRedraw = true;
}

bool lakatosGetDeveloperMode() {
  return gTouchSpotEnabled;
}

void lakatosSetDeveloperMode(bool en) {
  if (en) gCanBeDeveloper = true;
  gTouchSpotEnabled = en;
  gConfigNeedsRedraw = true;
}

bool lakatosGetTouchSpotEnabled() {
  return gTouchSpotEnabled;
}

void lakatosSetTouchSpotEnabled(bool en) {
  lakatosSetDeveloperMode(en);
}

int lakatosGetCornerSize() {
  return constrain(gCornerSize, 8, 24);
}

void lakatosSetCornerSize(int sz) {
  gCornerSize = snapCornerSize(sz);
  saveConfigToNvs();
  gConfigNeedsRedraw = true;
}

int lakatosGetActivityMode() {
  return gActivityMode;
}

void lakatosSetActivityMode(int mode) {
  if (mode < LAKATOS_MODE_AMBIENT || mode > LAKATOS_MODE_ACTIVE) {
    mode = LAKATOS_MODE_ACTIVE;
  }
  gActivityMode = mode;
}

const char* lakatosGetAmbientApp() {
  return gAmbientApp;
}

void lakatosSetAmbientApp(const char* appName) {
  if (!appName || !appName[0]) {
    strcpy(gAmbientApp, "ambient");
  } else {
    strncpy(gAmbientApp, appName, sizeof(gAmbientApp) - 1);
    gAmbientApp[sizeof(gAmbientApp) - 1] = '\0';
  }
  saveConfigToNvs();
}

int lakatosGetAmbientSec() {
  return constrain(gAmbientSec, 1, 86400);
}

void lakatosSetAmbientSec(int sec) {
  gAmbientSec = constrain(sec, 1, 86400);
  saveConfigToNvs();
}

int lakatosGetDemoSec() {
  return constrain(gDemoSec, 5, 3600);
}

void lakatosSetDemoSec(int sec) {
  gDemoSec = constrain(sec, 5, 3600);
  saveConfigToNvs();
}

int lakatosGetActiveIdleSec() {
  return constrain(gActiveIdleSec, 0, 86400);
}

void lakatosSetActiveIdleSec(int sec) {
  gActiveIdleSec = constrain(sec, 0, 86400);
  saveConfigToNvs();
}

bool lakatosWifiIsConnected() { return WiFi.status() == WL_CONNECTED; }

int64_t lakatosGetUnixTime() {
  time_t now = time(nullptr);
  return (int64_t)now;
}

int64_t lakatosGetKernelVersionSec() {
  return LAKATOS_KERNEL_VERSION_SEC;
}

bool lakatosFormatLocalTime(char* buf, size_t len, const char* fmt) {
  if (!buf || len == 0) return false;
  time_t now = time(nullptr);
  struct tm ti;
  if (now < 1700000000 || !localtime_r(&now, &ti)) {
    snprintf(buf, len, "--:--");
    return false;
  }
  strftime(buf, len, (fmt && fmt[0]) ? fmt : "%H:%M:%S", &ti);
  return true;
}

// ---------------------------------------------------------------------------
// Reusable Modal Stylus Keyboard
// ---------------------------------------------------------------------------
static const char* const KB_ROWS[3][4] = {
  { "1234567890", "qwertyuiop", "asdfghjkl/", "zxcvbnm._-" },
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL/", "ZXCVBNM._-" },
  { "!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "`~$%&*+=-_" }
};

static void drawKbInputBox(const String& text, int& cursor, int& viewStart,
                           int* outViewEnd = nullptr) {
  constexpr int kTextX0   = 20;
  constexpr int kTextMaxW = 278;

  int len = (int)text.length();
  if (cursor < 0) cursor = 0;
  if (cursor > len) cursor = len;
  if (viewStart < 0) viewStart = 0;
  if (viewStart > cursor) viewStart = cursor;

  LakatosTftGuard tftLock;
  while (viewStart < cursor &&
         tft.textWidth(text.substring(viewStart, cursor), 2) > kTextMaxW) {
    viewStart++;
  }
  while (viewStart > 0 &&
         tft.textWidth(text.substring(viewStart - 1, len), 2) <= kTextMaxW) {
    viewStart--;
  }

  int viewEnd = viewStart;
  while (viewEnd < len &&
         tft.textWidth(text.substring(viewStart, viewEnd + 1), 2) <= kTextMaxW) {
    viewEnd++;
  }
  if (outViewEnd) *outViewEnd = viewEnd;

  tft.fillRoundRect(4, 28, 312, 30, 4, TFT_BLACK);
  tft.drawRoundRect(4, 28, 312, 30, 4, TFT_CYAN);

  if (viewStart > 0) {
    tft.fillTriangle(8, 43, 16, 36, 16, 50, TFT_YELLOW);
  }
  if (viewEnd < len) {
    tft.fillTriangle(311, 43, 303, 36, 303, 50, TFT_YELLOW);
  }

  String visible = text.substring(viewStart, viewEnd);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(visible, kTextX0, 31, 2);

  int cursorPx = kTextX0 + tft.textWidth(text.substring(viewStart, cursor), 2);
  if (cursorPx < kTextX0) cursorPx = kTextX0;
  if (cursorPx > kTextX0 + kTextMaxW) cursorPx = kTextX0 + kTextMaxW;
  tft.fillTriangle(cursorPx, 48, cursorPx - 4, 55, cursorPx + 4, 55, TFT_CYAN);
}

static void drawKbGrid(int page) {
  LakatosTftGuard tftLock;
  tft.fillRect(0, 62, LAKATOS_SCREEN_W, LAKATOS_SCREEN_H - 62, 0x0841);

  for (int r = 0; r < 4; r++) {
    const char* rowStr = KB_ROWS[page][r];
    int ky = 64 + r * 35;
    for (int c = 0; c < 10; c++) {
      int kx = 2 + c * 32;
      char ch[2] = {rowStr[c], '\0'};
      tft.fillRoundRect(kx, ky, 29, 32, 4, 0x18E3);
      tft.drawRoundRect(kx, ky, 29, 32, 4, 0x4A69);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, 0x18E3);
      tft.drawString(ch, kx + 14, ky + 16, 2);
    }
  }

  int by = 205, bh = 32;
  uint16_t shiftCol = (page == 1) ? TFT_YELLOW : 0x2945;
  tft.fillRoundRect(2, by, 54, bh, 4, shiftCol);
  tft.drawRoundRect(2, by, 54, bh, 4, TFT_YELLOW);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor((page == 1) ? TFT_BLACK : TFT_YELLOW, shiftCol);
  tft.drawString((page == 1) ? "abc" : "ABC", 29, by + 16, 2);

  uint16_t symCol = (page == 2) ? TFT_MAGENTA : 0x2945;
  tft.fillRoundRect(60, by, 54, bh, 4, symCol);
  tft.drawRoundRect(60, by, 54, bh, 4, TFT_MAGENTA);
  tft.setTextColor((page == 2) ? TFT_BLACK : TFT_MAGENTA, symCol);
  tft.drawString((page == 2) ? "abc" : "#+=", 87, by + 16, 2);

  tft.fillRoundRect(118, by, 78, bh, 4, 0x18E3);
  tft.drawRoundRect(118, by, 78, bh, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("SPACE", 157, by + 16, 2);

  tft.fillRoundRect(200, by, 54, bh, 4, 0x4800);
  tft.drawRoundRect(200, by, 54, bh, 4, TFT_RED);
  tft.setTextColor(TFT_WHITE, 0x4800);
  tft.drawString("DEL", 227, by + 16, 2);

  tft.fillRoundRect(258, by, 59, bh, 4, TFT_DARKGREEN);
  tft.drawRoundRect(258, by, 59, bh, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("OK", 287, by + 16, 2);
}

static void drawGraffitiPad(int grafShift) {
  LakatosTftGuard tftLock;
  tft.fillRect(0, 62, LAKATOS_SCREEN_W, LAKATOS_SCREEN_H - 62, 0x0841);

  // Left Pane: LETTERS (a-z) (x: 34..154, w: 120, y: 64..201)
  tft.fillRoundRect(34, 64, 120, 137, 5, TFT_BLACK);
  tft.drawRoundRect(34, 64, 120, 137, 5, TFT_MAGENTA);

  // Right Pane: DIGITS (0-9) & MATH (x: 166..286, w: 120, y: 64..201)
  tft.fillRoundRect(166, 64, 120, 137, 5, TFT_BLACK);
  tft.drawRoundRect(166, 64, 120, 137, 5, TFT_YELLOW);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("LETTERS (a-z)", 94, 69, 1);
  tft.drawString("SP:-> BS:<- SH:^", 94, 189, 1);

  tft.drawString("DIGITS (0-9)", 226, 69, 1);
  tft.drawString("+ - * / = ( ) .", 226, 189, 1);

  // Bottom bar (y: 205..237): [abc/Abc/ABC] [SPACE] [DEL] [?] [OK]
  int by = 205, bh = 32;
  uint16_t shBg  = (grafShift == 2) ? TFT_YELLOW : ((grafShift == 1) ? 0x4208 : 0x2945);
  uint16_t shFg  = (grafShift == 2) ? TFT_BLACK : TFT_YELLOW;
  const char* shLbl = (grafShift == 2) ? "ABC" : ((grafShift == 1) ? "Abc" : "abc");
  tft.fillRoundRect(2, by, 60, bh, 4, shBg);
  tft.drawRoundRect(2, by, 60, bh, 4, TFT_YELLOW);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(shFg, shBg);
  tft.drawString(shLbl, 32, by + 16, 2);

  tft.fillRoundRect(66, by, 78, bh, 4, 0x18E3);
  tft.drawRoundRect(66, by, 78, bh, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("SPACE", 105, by + 16, 2);

  tft.fillRoundRect(148, by, 54, bh, 4, 0x4800);
  tft.drawRoundRect(148, by, 54, bh, 4, TFT_RED);
  tft.setTextColor(TFT_WHITE, 0x4800);
  tft.drawString("DEL", 175, by + 16, 2);

  tft.fillRoundRect(206, by, 44, bh, 4, 0x2945);
  tft.drawRoundRect(206, by, 44, bh, 4, TFT_MAGENTA);
  tft.setTextColor(TFT_MAGENTA, 0x2945);
  tft.drawString("?", 228, by + 16, 2);

  tft.fillRoundRect(254, by, 63, bh, 4, TFT_DARKGREEN);
  tft.drawRoundRect(254, by, 63, bh, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("OK", 285, by + 16, 2);
}

static void drawKeyboardModeMenu(bool isGraf) {
  int sz    = lakatosGetCornerSize();
  int menuY = (sz < 24) ? 24 : sz;
  LakatosTftGuard tftLock;
  tft.fillRoundRect(204, menuY, 114, 61, 5, 0x10A2);
  tft.drawRoundRect(204, menuY, 114, 61, 5, TFT_CYAN);

  tft.setTextDatum(MC_DATUM);
  const char* const items[2] = {"Keyboard", "Graffiti"};
  for (int i = 0; i < 2; i++) {
    int iy   = menuY + 3 + i * 29;
    bool sel = ((i == 1) == isGraf);
    uint16_t bg     = sel ? 0x1A26 : 0x18E3;
    uint16_t border = sel ? TFT_GREEN : TFT_CYAN;
    tft.fillRoundRect(208, iy, 106, 25, 4, bg);
    tft.drawRoundRect(208, iy, 106, 25, 4, border);
    tft.setTextColor(sel ? TFT_YELLOW : TFT_WHITE, bg);
    tft.drawString(items[i], 261, iy + 12, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

static void drawKeyboardModalHeader(const char* title, bool allowCtrlC,
                                    const String& text, int& cursor, int& viewStart) {
  LakatosTftGuard tftLock;
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 25, TFT_NAVY);

  tft.fillRoundRect(26, 2, 56, 20, 4, 0x2945);
  tft.drawRoundRect(26, 2, 56, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("CANCEL", 54, 12, 1);

  if (allowCtrlC) {
    tft.fillRoundRect(86, 2, 44, 20, 4, 0xA800);
    tft.drawRoundRect(86, 2, 44, 20, 4, TFT_RED);
    tft.setTextColor(TFT_WHITE, 0xA800);
    tft.drawString("^C", 108, 12, 2);
  }

  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString(title ? title : "INPUT", allowCtrlC ? 214 : 190, 12, 2);

  // Standard NE corner downward-pointing Menu Wedge
  lakatosDrawMenuWedge();

  drawKbInputBox(text, cursor, viewStart);
}

int lakatosPromptKeyboard(const char* title, char* buf, size_t maxLen,
                          bool allowCtrlC) {
  if (!buf || maxLen == 0) return 0;
  struct CornerExitSuspendGuard {
    bool prev;
    CornerExitSuspendGuard() : prev(lakatosGetCornerExitEnabled()) {
      lakatosSetCornerExitEnabled(false);
    }
    ~CornerExitSuspendGuard() {
      lakatosSetCornerExitEnabled(prev);
    }
  } cornerGuard;
  String text = String(buf);
  int cursor = (int)text.length();
  int viewStart = 0;
  int page = 0;
  int grafShift = 0;  // 0 = abc, 1 = Abc (one-shot), 2 = ABC (caps lock)
  bool menuOpen = false;

  auto insertCharAtCursor = [&](char ch) {
    if (text.length() + 1 < maxLen) {
      text = text.substring(0, cursor) + ch + text.substring(cursor);
      cursor++;
      drawKbInputBox(text, cursor, viewStart);
    }
  };

  auto deleteCharAtCursor = [&]() {
    if (cursor > 0 && text.length() > 0) {
      text = text.substring(0, cursor - 1) + text.substring(cursor);
      cursor--;
      drawKbInputBox(text, cursor, viewStart);
    }
  };

  drawKeyboardModalHeader(title, allowCtrlC, text, cursor, viewStart);
  if (gCal.kbGraf) {
    drawGraffitiPad(grafShift);
  } else {
    drawKbGrid(page);
  }

  // Wait for touch release before accepting new taps
  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastDownMs = 0;
  uint32_t lastPressMs = 0;
  uint32_t lastRel = millis();

  while (true) {
    if (lakatosShouldExitApp()) {
      return -1;
    }
    updateLakatosConfigBackground();
    LakatosTouchSample t = lakatosReadTouch();
    if (lakatosShouldExitApp()) {
      return -1;
    }
    uint32_t now = millis();

    if (t.touched) {
      lastDownMs = now;

      // In Graffiti mode, touches in the drawing area (y: 64..201) start stabilized
      // continuous stroke capture with outlier/jump rejection and a 75ms pen-up window.
      if (gCal.kbGraf && !menuOpen && !wasDown && t.y >= 64 && t.y < 202 && t.x >= 4 && t.x <= 316) {
        // 1. Stabilize initial pen-down by requiring 2 consecutive samples within 18 px
        int initCandX = constrain(t.x, 5, 315);
        int initCandY = constrain(t.y, 65, 200);
        int startX = initCandX, startY = initCandY;
        bool stabilized = false;
        uint32_t settleStartMs = now;
        while ((uint32_t)(millis() - settleStartMs) <= 60u) {
          delay(6);
          LakatosTouchSample s0 = lakatosReadTouch();
          if (s0.touched && s0.y >= 64 && s0.y < 202 && s0.x >= 4 && s0.x <= 316) {
            int sx0 = constrain(s0.x, 5, 315);
            int sy0 = constrain(s0.y, 65, 200);
            if (abs(sx0 - initCandX) + abs(sy0 - initCandY) <= 18) {
              startX = (initCandX + sx0) / 2;
              startY = (initCandY + sy0) / 2;
              stabilized = true;
              break;
            }
            initCandX = sx0;
            initCandY = sy0;
          }
        }
        if (!stabilized) {
          continue;
        }

        uint8_t pane = (startX < 160) ? LAKATOS_PANE_LETTERS : LAKATOS_PANE_DIGITS;
        uint16_t inkCol = (pane == LAKATOS_PANE_LETTERS) ? TFT_CYAN : TFT_YELLOW;
        LakatosGraffitiPt pts[128];
        pts[0] = {(int16_t)startX, (int16_t)startY};
        int ptCount = 1;
        int lastX = startX, lastY = startY;
        bool hasCand = false;
        int candX = startX, candY = startY;
        uint32_t lastContactMs = millis();

        {
          LakatosTftGuard tftLock;
          tft.fillCircle(lastX, lastY, 2, TFT_GREEN);
        }

        while (true) {
          if (lakatosShouldExitApp()) return -1;
          updateLakatosConfigBackground();
          LakatosTouchSample s = lakatosReadTouch();
          uint32_t sNow = millis();
          if (s.touched) {
            lastContactMs = sNow;
            int sx = constrain(s.x, 5, 315);
            int sy = constrain(s.y, 65, 200);
            int dLast = abs(sx - lastX) + abs(sy - lastY);

            if (hasCand) {
              int dCand     = abs(sx - candX) + abs(sy - candY);
              int dCandLast = abs(candX - lastX) + abs(candY - lastY);
              if (dCand <= 24 && dCandLast <= 36) {
                // Sample (sx, sy) confirms (candX, candY) is on a smooth path
                int commitX = (lastX + candX * 3) / 4;
                int commitY = (lastY + candY * 3) / 4;
                int dx = commitX - lastX;
                int dy = commitY - lastY;
                if (dx * dx + dy * dy >= 4) {
                  {
                    LakatosTftGuard tftLock;
                    tft.drawLine(lastX, lastY, commitX, commitY, inkCol);
                  }
                  if (ptCount < 128) {
                    pts[ptCount++] = {(int16_t)commitX, (int16_t)commitY};
                  }
                  lastX = commitX;
                  lastY = commitY;
                }
                candX = sx;
                candY = sy;
              } else if (dLast <= 28) {
                // (candX, candY) was a 1-sample wild jump; (sx, sy) returned near (lastX, lastY)
                candX = sx;
                candY = sy;
              } else {
                // Wild jump: discard unconfirmed candidate
                hasCand = false;
              }
            } else {
              if (dLast <= 36) {
                hasCand = true;
                candX   = sx;
                candY   = sy;
              }
            }
          } else if (digitalRead(XPT2046_IRQ) == LOW) {
            lastContactMs = sNow;
          } else if ((uint32_t)(sNow - lastContactMs) >= 75u) {
            break;
          }
          delay(8);
        }

        // Only commit final lookahead point if it didn't jump at pen lift-off
        if (hasCand && (abs(candX - lastX) + abs(candY - lastY)) <= 10 && ptCount < 128) {
          int dx = candX - lastX;
          int dy = candY - lastY;
          if (dx * dx + dy * dy >= 4) {
            pts[ptCount++] = {(int16_t)candX, (int16_t)candY};
          }
        }

        LakatosGraffitiResult gr = lakatosRecognizeGraffitiStroke(pts, ptCount, pane);
        if (gr.ch == LAKATOS_GRAFFITI_BACKSPACE) {
          deleteCharAtCursor();
        } else if (gr.ch == LAKATOS_GRAFFITI_SHIFT) {
          grafShift = (grafShift + 1) % 3;
        } else if (gr.ch >= 32 && gr.ch <= 126) {
          char outCh = gr.ch;
          if (outCh >= 'a' && outCh <= 'z' && grafShift > 0) {
            outCh = (char)(outCh - 32);
            if (grafShift == 1) grafShift = 0;
          }
          insertCharAtCursor(outCh);
        }

        drawGraffitiPad(grafShift);
        wasDown = false;
        lastRel = millis();
        lastPressMs = lastRel;
        continue;
      }

      if (!wasDown && (uint32_t)(now - lastRel) >= 95u &&
          (lastPressMs == 0 || (uint32_t)(now - lastPressMs) >= 180u)) {
        wasDown = true;
        lastPressMs = now;
        if (menuOpen) {
          int sz    = lakatosGetCornerSize();
          int menuY = (sz < 24) ? 24 : sz;
          menuOpen = false;
          if (t.x >= 204 && t.x <= 318 && t.y >= menuY + 2 && t.y < menuY + 60) {
            int itemIdx = (t.y - (menuY + 3)) / 29;
            bool nextGraf = (itemIdx == 1);
            if (nextGraf != gCal.kbGraf) {
              gCal.kbGraf = nextGraf;
              saveConfigToNvs();
            }
          }
          drawKbInputBox(text, cursor, viewStart);
          if (gCal.kbGraf) {
            drawGraffitiPad(grafShift);
          } else {
            drawKbGrid(page);
          }
          continue;
        }
        {
          int sz    = lakatosGetCornerSize();
          int hitSz = (sz < 24) ? 24 : sz;
          if (t.y <= hitSz && t.x >= LAKATOS_SCREEN_W - hitSz) {
            menuOpen = true;
            drawKeyboardModeMenu(gCal.kbGraf);
            continue;
          }
        }
        if (t.y <= 26 && t.x >= 24 && t.x <= 84) {
          lakatosWaitTouchRelease();
          return 0;  // Cancelled
        }
        if (allowCtrlC && t.y <= 26 && t.x >= 85 && t.x <= 132) {
          lakatosWaitTouchRelease();
          return -1;  // ^C
        }
        if (t.y >= 27 && t.y <= 60) {
          // Tap in edit line box: left/right wedges scroll by 20 chars; otherwise
          // hold-and-glide moves the cyan cursor wedge to the closest inter-character gap.
          int len = (int)text.length();
          int viewEnd = viewStart;
          {
            LakatosTftGuard tftLock;
            while (viewEnd < len &&
                   tft.textWidth(text.substring(viewStart, viewEnd + 1), 2) <= 278) {
              viewEnd++;
            }
          }
          if (viewStart > 0 && t.x < 22) {
            cursor -= 20;
            if (cursor < 0) cursor = 0;
            drawKbInputBox(text, cursor, viewStart);
          } else if (viewEnd < len && t.x > 298) {
            cursor += 20;
            if (cursor > len) cursor = len;
            drawKbInputBox(text, cursor, viewStart);
          } else {
            auto snapCursorToX = [&](int px) {
              int bestIdx  = viewStart;
              int bestDist = 9999;
              {
                LakatosTftGuard tftLock;
                for (int i = viewStart; i <= viewEnd; i++) {
                  int gx = 20 + tft.textWidth(text.substring(viewStart, i), 2);
                  int d  = abs(px - gx);
                  if (d < bestDist) {
                    bestDist = d;
                    bestIdx  = i;
                  }
                }
              }
              if (bestIdx != cursor) {
                cursor = bestIdx;
                drawKbInputBox(text, cursor, viewStart, &viewEnd);
              }
            };
            snapCursorToX(t.x);
            int lastDragX = t.x;
            uint32_t lastContactMs = now;
            while (true) {
              if (lakatosShouldExitApp()) return -1;
              updateLakatosConfigBackground();
              LakatosTouchSample s = lakatosReadTouch();
              uint32_t sNow = millis();
              if (s.touched) {
                lastContactMs = sNow;
                if (s.y >= 18 && s.y <= 76 && abs(s.x - lastDragX) <= 44) {
                  lastDragX = s.x;
                  snapCursorToX(s.x);
                }
              } else if (digitalRead(XPT2046_IRQ) == LOW) {
                lastContactMs = sNow;
              } else if ((uint32_t)(sNow - lastContactMs) >= 60u) {
                break;
              }
              delay(10);
            }
            wasDown = false;
            lastRel = millis();
            lastPressMs = lastRel;
          }
        } else if (!gCal.kbGraf && t.y >= 64 && t.y < 204) {
          int r = (t.y - 64) / 35;
          int c = t.x / 32;
          if (r >= 0 && r < 4 && c >= 0 && c < 10) {
            insertCharAtCursor(KB_ROWS[page][r][c]);
          }
        } else if (t.y >= 205) {
          if (!gCal.kbGraf) {
            // QWERTY bottom bar: [ABC] (2..56) [#+=] (60..114) [SPACE] (118..196) [DEL] (200..254) [OK] (258..317)
            if (t.x < 58) {
              page = (page == 1) ? 0 : 1;
              drawKbGrid(page);
            } else if (t.x < 116) {
              page = (page == 2) ? 0 : 2;
              drawKbGrid(page);
            } else if (t.x < 198) {
              insertCharAtCursor(' ');
            } else if (t.x < 256) {
              deleteCharAtCursor();
            } else {
              strncpy(buf, text.c_str(), maxLen - 1);
              buf[maxLen - 1] = '\0';
              lakatosWaitTouchRelease();
              return 1;
            }
          } else {
            // Graffiti bottom bar: [Shift] (2..62) [SPACE] (66..144) [DEL] (148..202) [?] (206..250) [OK] (254..317)
            if (t.x < 64) {
              grafShift = (grafShift + 1) % 3;
              drawGraffitiPad(grafShift);
            } else if (t.x < 146) {
              insertCharAtCursor(' ');
            } else if (t.x < 204) {
              deleteCharAtCursor();
            } else if (t.x < 252) {
              lakatosRunGraffitiHelpModal();
              drawKeyboardModalHeader(title, allowCtrlC, text, cursor, viewStart);
              drawGraffitiPad(grafShift);
              wasDown = false;
              lastRel = millis();
            } else {
              strncpy(buf, text.c_str(), maxLen - 1);
              buf[maxLen - 1] = '\0';
              lakatosWaitTouchRelease();
              return 1;
            }
          }
        }
      }
    } else if (digitalRead(XPT2046_IRQ) == LOW && wasDown) {
      lastDownMs = now;
    } else {
      if (wasDown && (uint32_t)(now - lastDownMs) >= 95u) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(12);
  }
}

// ---------------------------------------------------------------------------
// 3-Point Touchscreen Calibration Modal
// ---------------------------------------------------------------------------
static void drawCrosshair(int x, int y, uint16_t color, const char* label) {
  LakatosTftGuard tftLock;
  tft.drawCircle(x, y, 16, color);
  tft.drawCircle(x, y, 8, color);
  tft.fillCircle(x, y, 3, TFT_WHITE);
  tft.drawFastHLine(x - 22, y, 44, color);
  tft.drawFastVLine(x, y - 22, 44, color);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, 160, 125, 2);
}

static void runTouchCalibrationModal() {
  int rawX[3] = {0, 0, 0};
  int rawY[3] = {0, 0, 0};
  const int tx[3] = {gCal.calX1, gCal.calX2, gCal.calX2};
  const int ty[3] = {gCal.calY1, gCal.calY1, gCal.calY2};
  const char* labels[3] = {"TAP 1: TOP-LEFT", "TAP 2: TOP-RIGHT", "TAP 3: BOTTOM-RIGHT"};
  const uint16_t cols[3] = {TFT_CYAN, TFT_YELLOW, TFT_GREEN};

  for (int step = 0; step < 3; step++) {
    {
      LakatosTftGuard tftLock;
      tft.fillScreen(TFT_BLACK);
      tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);
      tft.setTextColor(TFT_WHITE, TFT_NAVY);
      tft.setTextDatum(MC_DATUM);
      tft.drawString("LAKATOS TOUCH CALIBRATION", 160, 12, 2);
      drawCrosshair(tx[step], ty[step], cols[step], labels[step]);
    }

    lakatosWaitTouchRelease();
    while (true) {
      updateLakatosConfigBackground();
      if (lakatosShouldExitApp()) return;
      LakatosTouchSample t = lakatosReadTouch();
      if (t.touched) {
        long sumX = t.rawX;
        long sumY = t.rawY;
        int  cnt  = 1;
        for (int s = 0; s < 5; s++) {
          delay(12);
          LakatosTouchSample t2 = lakatosReadTouch();
          if (t2.touched) {
            sumX += t2.rawX;
            sumY += t2.rawY;
            cnt++;
          }
        }
        rawX[step] = (int)(sumX / cnt);
        rawY[step] = (int)(sumY / cnt);
        lakatosSetRgbLed(false, true, false, 80);
        lakatosWaitTouchRelease();
        break;
      }
      delay(15);
    }
  }

  int dx12 = abs(rawX[1] - rawX[0]);
  int dy12 = abs(rawY[1] - rawY[0]);
  gCal.swapXY = (dy12 > dx12);
  gCal.uAtX1  = gCal.swapXY ? (float)rawY[0] : (float)rawX[0];
  gCal.uAtX2  = gCal.swapXY ? (float)rawY[1] : (float)rawX[1];
  gCal.vAtY1  = gCal.swapXY ? (float)(rawX[0] + rawX[1]) * 0.5f : (float)(rawY[0] + rawY[1]) * 0.5f;
  gCal.vAtY2  = gCal.swapXY ? (float)rawX[2] : (float)rawY[2];
  saveTouchCalibrationToNvs();
  Serial.printf("[Lakatos] Saved touch calibration: swapXY=%d u=[%.0f..%.0f] v=[%.0f..%.0f]\r\n",
                gCal.swapXY ? 1 : 0, gCal.uAtX1, gCal.uAtX2, gCal.vAtY1, gCal.vAtY2);
}

// ---------------------------------------------------------------------------
// Wi-Fi Network Scanner & Config Modal
// ---------------------------------------------------------------------------
static bool runWifiConfigModal() {
  static const int kMaxWifiList = 20;
  String ssids[kMaxWifiList];
  int32_t rssis[kMaxWifiList];
  int count = 0;
  int page = 0;

  auto scanNow = [&]() {
    {
      LakatosTftGuard tftLock;
      tft.fillScreen(0x0841);
      tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_NAVY);
      tft.drawString("SCANNING WI-FI...", 160, 12, 2);
    }
    WiFi.mode(WIFI_STA);
    // If not currently connected, stop any background connection/auto-reconnect
    // attempt; otherwise ESP-IDF esp_wifi_scan_start() returns ESP_ERR_WIFI_STATE
    // ("sta is connecting, scan are not allowed") and scanNetworks() returns -2.
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(false, false);
      delay(120);
    }
    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false, false);
    if (n < 0) {
      lakatosDevLogf("Wi-Fi scan returned %d; resetting STA and retrying...", n);
      WiFi.disconnect(true, false);
      delay(200);
      WiFi.mode(WIFI_STA);
      delay(120);
      n = WiFi.scanNetworks(false, false);
    }
    count = 0;
    for (int i = 0; i < n && count < kMaxWifiList; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      bool dup = false;
      for (int j = 0; j < count; j++) {
        if (ssids[j] == s) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        ssids[count] = s;
        rssis[count] = WiFi.RSSI(i);
        count++;
      }
    }
    WiFi.scanDelete();
    page = 0;
    lakatosDevLogf("Wi-Fi scan: raw=%d unique=%d", n, count);
    for (int i = 0; i < count; i++) {
      lakatosDevLogf("  [%d] '%s' (%ld dBm)", i, ssids[i].c_str(), (long)rssis[i]);
    }
  };

  auto resumeSavedWifiIfDisconnected = [&]() {
    if (WiFi.status() == WL_CONNECTED) return;
    prefs.begin("cyd_wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    if (ssid.length() > 0) {
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      if (pass.length() > 0) {
        WiFi.begin(ssid.c_str(), pass.c_str());
      } else {
        WiFi.begin(ssid.c_str());
      }
    }
  };

  auto drawWifiList = [&]() {
    int totalPages = (count > 0) ? ((count + 4) / 5) : 1;
    if (page >= totalPages) page = 0;

    LakatosTftGuard tftLock;
    tft.fillScreen(0x0841);
    tft.fillRect(0, 0, LAKATOS_SCREEN_W, 25, TFT_NAVY);
    tft.fillRoundRect(2, 2, 58, 20, 4, 0x2945);
    tft.drawRoundRect(2, 2, 58, 20, 4, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x2945);
    tft.drawString("HOME", 31, 12, 2);

    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    if (totalPages > 1) {
      tft.drawString("WI-FI " + String(page + 1) + "/" + String(totalPages), 122, 12, 2);
      tft.fillRoundRect(188, 2, 62, 20, 4, 0x2945);
      tft.drawRoundRect(188, 2, 62, 20, 4, TFT_CYAN);
      tft.setTextColor(TFT_CYAN, 0x2945);
      tft.drawString("PAGE >", 219, 12, 1);
    } else {
      tft.drawString("WI-FI NETWORKS", 155, 12, 2);
    }

    tft.fillRoundRect(254, 2, 64, 20, 4, 0x2945);
    tft.drawRoundRect(254, 2, 64, 20, 4, TFT_YELLOW);
    tft.setTextColor(TFT_YELLOW, 0x2945);
    tft.drawString("RESCAN", 286, 12, 1);

    tft.setTextDatum(ML_DATUM);
    if (WiFi.status() == WL_CONNECTED) {
      tft.setTextColor(TFT_GREEN, 0x0841);
      tft.drawString("Connected: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")", 6, 36, 1);
    } else if (count > 0) {
      tft.setTextColor(TFT_ORANGE, 0x0841);
      tft.drawString("Found " + String(count) + " networks - tap to connect:", 6, 36, 1);
    } else {
      tft.setTextColor(TFT_ORANGE, 0x0841);
      tft.drawString("No networks found - tap RESCAN:", 6, 36, 1);
    }

    int startIdx = page * 5;
    for (int row = 0; row < 5 && (startIdx + row) < count; row++) {
      int i = startIdx + row;
      int ry = 48 + row * 36;
      tft.fillRoundRect(6, ry, 308, 32, 5, 0x18E3);
      tft.drawRoundRect(6, ry, 308, 32, 5, TFT_CYAN);
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(TFT_WHITE, 0x18E3);
      tft.drawString(ssids[i].substring(0, 24), 14, ry + 16, 2);
      tft.setTextDatum(MR_DATUM);
      tft.setTextColor(TFT_LIGHTGREY, 0x18E3);
      tft.drawString(String(rssis[i]) + "dBm", 306, ry + 16, 1);
    }
  };

  scanNow();
  drawWifiList();
  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    updateLakatosConfigBackground();
    if (lakatosShouldExitApp()) {
      resumeSavedWifiIfDisconnected();
      return true;
    }
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        if (t.y <= 25) {
          if (t.x <= 62) {
            resumeSavedWifiIfDisconnected();
            return true;
          }
          if (t.x >= 186 && t.x < 252 && count > 5) {
            int totalPages = (count + 4) / 5;
            page = (page + 1) % totalPages;
            drawWifiList();
          } else if (t.x >= 252) {
            scanNow();
            drawWifiList();
          }
        } else if (t.y >= 48) {
          int row = (t.y - 48) / 36;
          int idx = page * 5 + row;
          if (row >= 0 && row < 5 && idx >= 0 && idx < count) {
            char passBuf[64] = "";
            String title = "PW: " + ssids[idx].substring(0, 14);
            if (lakatosPromptKeyboard(title.c_str(), passBuf, sizeof(passBuf), false) == 1) {
              prefs.begin("cyd_wifi", false);
              prefs.putString("ssid", ssids[idx]);
              prefs.putString("pass", passBuf);
              prefs.end();
              {
                LakatosTftGuard tftLock;
                tft.fillScreen(0x0841);
                tft.setTextDatum(MC_DATUM);
                tft.setTextColor(TFT_YELLOW, 0x0841);
                tft.drawString("Connecting to " + ssids[idx] + "...", 160, 120, 2);
              }
              WiFi.disconnect(false, false);
              delay(100);
              WiFi.setSleep(false);
              if (strlen(passBuf) > 0) {
                WiFi.begin(ssids[idx].c_str(), passBuf);
              } else {
                WiFi.begin(ssids[idx].c_str());
              }
              uint32_t startMs = millis();
              while (WiFi.status() != WL_CONNECTED && (millis() - startMs < 9000)) {
                delay(200);
              }
              return false;
            }
            drawWifiList();
          }
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

// ---------------------------------------------------------------------------
// Airport Selector Modal
// ---------------------------------------------------------------------------
static bool runAirportSelectorModal() {
  auto drawAirportModal = [&]() {
    LakatosTftGuard tftLock;
    tft.fillScreen(0x0841);
    tft.fillRect(0, 0, LAKATOS_SCREEN_W, 25, TFT_NAVY);

    tft.fillRoundRect(2, 2, 58, 20, 4, 0x2945);
    tft.drawRoundRect(2, 2, 58, 20, 4, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x2945);
    tft.drawString("HOME", 31, 12, 2);

    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("SELECT AIRPORT", 145, 12, 2);

    tft.fillRoundRect(232, 2, 85, 20, 4, 0x2945);
    tft.drawRoundRect(232, 2, 85, 20, 4, TFT_YELLOW);
    tft.setTextColor(TFT_YELLOW, 0x2945);
    tft.drawString("TYPE CODE", 274, 12, 1);

    for (int i = 0; i < 12; i++) {
      int r = i / 4;
      int c = i % 4;
      int bx = 6 + c * 78;
      int by = 36 + r * 64;
      bool sel = (strcmp(QUICK_AIRPORTS[i], gAirportCode) == 0);
      uint16_t bg = sel ? TFT_DARKGREEN : 0x18E3;
      uint16_t fg = sel ? TFT_GREEN : TFT_CYAN;
      tft.fillRoundRect(bx, by, 72, 56, 6, bg);
      tft.drawRoundRect(bx, by, 72, 56, 6, fg);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, bg);
      tft.drawString(QUICK_AIRPORTS[i], bx + 36, by + 28, 4);
    }
  };

  drawAirportModal();
  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    updateLakatosConfigBackground();
    if (lakatosShouldExitApp()) {
      return true;
    }
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        if (t.y <= 25) {
          if (t.x <= 62) return true;
          if (t.x >= 230) {
            char codeBuf[8] = "";
            strncpy(codeBuf, gAirportCode, sizeof(codeBuf) - 1);
            if (lakatosPromptKeyboard("3-LETTER IATA", codeBuf, 4, false) == 1 &&
                strlen(codeBuf) == 3) {
              applyAirportCode(codeBuf);
              saveConfigToNvs();
              return false;
            }
            drawAirportModal();
          }
        } else if (t.y >= 36) {
          int r = (t.y - 36) / 64;
          int c = (t.x - 6) / 78;
          if (r >= 0 && r < 3 && c >= 0 && c < 4) {
            applyAirportCode(QUICK_AIRPORTS[r * 4 + c]);
            saveConfigToNvs();
            return false;
          }
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

// ---------------------------------------------------------------------------
// STORAGE Status Screen (/flash & /sd Mounted State and Disk Free)
// ---------------------------------------------------------------------------
static void formatStorageBytes(uint64_t bytes, char* out, size_t cap) {
  if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
    snprintf(out, cap, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
  } else if (bytes >= 10ULL * 1024ULL * 1024ULL) {
    snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL * 1024ULL) {
    snprintf(out, cap, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL) {
    snprintf(out, cap, "%llu KB", (unsigned long long)(bytes / 1024ULL));
  } else {
    snprintf(out, cap, "%llu B", (unsigned long long)bytes);
  }
}

static void drawStoragePartitionCard(int cardY, const char* title, bool mounted,
                                     bool calculating,
                                     uint64_t totalB, uint64_t usedB, uint64_t freeB,
                                     const char* unmountedMsg, const char* unmountedHint) {
  LakatosTftGuard tftLock;
  tft.fillRoundRect(6, cardY, 308, 98, 6, 0x1082);
  tft.drawRoundRect(6, cardY, 308, 98, 6, (mounted || calculating) ? TFT_CYAN : 0x7BEF);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, 0x1082);
  tft.drawString(title, 14, cardY + 5, 2);

  uint16_t badgeBg     = calculating ? (mounted ? TFT_DARKGREEN : 0x2945) : (mounted ? TFT_DARKGREEN : TFT_MAROON);
  uint16_t badgeBorder = calculating ? (mounted ? TFT_GREEN : TFT_CYAN)   : (mounted ? TFT_GREEN : TFT_RED);
  const char* badgeTxt = calculating ? (mounted ? "MOUNTED" : "CHECKING...")
                                     : (mounted ? "MOUNTED" : "NOT MOUNTED");
  tft.fillRoundRect(216, cardY + 5, 90, 18, 4, badgeBg);
  tft.drawRoundRect(216, cardY + 5, 90, 18, 4, badgeBorder);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, badgeBg);
  tft.drawString(badgeTxt, 261, cardY + 14, 1);

  tft.drawFastHLine(14, cardY + 26, 292, 0x39E7);

  if (calculating) {
    tft.fillRect(14, cardY + 32, 292, 16, 0x0841);
    tft.drawRect(14, cardY + 32, 292, 16, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x0841);
    tft.drawString("Calculating free space...", 160, cardY + 40, 1);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString("Disk Free:", 14, cardY + 53, 2);
    tft.setTextColor(TFT_YELLOW, 0x1082);
    tft.drawString("Calculating...", 104, cardY + 53, 2);

    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString("Used/Total:", 14, cardY + 74, 2);
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString("Scanning partition...", 104, cardY + 74, 2);
    return;
  }

  if (mounted && totalB > 0) {
    int usedPct = (int)((usedB * 100ULL + totalB / 2ULL) / totalB);
    if (usedPct < 0) usedPct = 0;
    if (usedPct > 100) usedPct = 100;
    int freePct = 100 - usedPct;

    tft.fillRect(14, cardY + 32, 292, 14, 0x0841);
    tft.drawRect(14, cardY + 32, 292, 14, 0x39E7);
    int fillW = (int)((usedB * 290ULL) / totalB);
    if (fillW > 290) fillW = 290;
    if (fillW > 0) {
      tft.fillRect(15, cardY + 33, fillW, 12, (usedPct >= 85) ? TFT_ORANGE : 0x04FF);
    }

    char freeStr[24], usedStr[24], totStr[24], line1[64], line2[64];
    formatStorageBytes(freeB, freeStr, sizeof(freeStr));
    formatStorageBytes(usedB, usedStr, sizeof(usedStr));
    formatStorageBytes(totalB, totStr, sizeof(totStr));
    snprintf(line1, sizeof(line1), "%s  (%d%% free)", freeStr, freePct);
    snprintf(line2, sizeof(line2), "%s / %s  (%d%% used)", usedStr, totStr, usedPct);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString("Disk Free:", 14, cardY + 51, 2);
    tft.setTextColor(TFT_GREEN, 0x1082);
    tft.drawString(line1, 104, cardY + 51, 2);

    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString("Used/Total:", 14, cardY + 73, 2);
    tft.setTextColor(TFT_WHITE, 0x1082);
    tft.drawString(line2, 104, cardY + 73, 2);
  } else {
    tft.fillRect(14, cardY + 32, 292, 16, 0x0841);
    tft.drawRect(14, cardY + 32, 292, 16, 0x39E7);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_ORANGE, 0x0841);
    tft.drawString(unmountedMsg, 160, cardY + 40, 1);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString("Disk Free:", 14, cardY + 53, 2);
    tft.setTextColor(TFT_YELLOW, 0x1082);
    tft.drawString("--  (Not Mounted)", 104, cardY + 53, 2);

    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString("Status:", 14, cardY + 74, 2);
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString(unmountedHint, 104, cardY + 74, 2);
  }
}

static void drawStorageScreen(bool forceProbeSd) {
  // 1. Render page skeleton and "Calculating..." placeholders immediately
  {
    LakatosTftGuard tftLock;
    tft.fillScreen(0x0841);
    tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);

    // [ HOME ] (top-left -> goes back to Home page)
    tft.fillRoundRect(2, 2, 58, 20, 4, 0x2945);
    tft.drawRoundRect(2, 2, 58, 20, 4, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x2945);
    tft.drawString("HOME", 31, 12, 2);

    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("STORAGE", 160, 12, 2);
  }

  drawStoragePartitionCard(
      30, "Flash  (/flash - LittleFS)", false, true,
      0, 0, 0,
      "Internal LittleFS partition not mounted",
      "Check spiffs partition table");
  drawStoragePartitionCard(
      134, "SD Card  (/sd - MicroSD)", false, true,
      0, 0, 0,
      "No MicroSD card detected in slot",
      "No card present");

  // 2. Calculate /flash first (fast) and update the /flash card immediately
  LakatosStorageInfo info;
  memset(&info, 0, sizeof(info));
  info.sdCardType = "None";
  lakatosGetFlashStorageInfo(&info);
  drawStoragePartitionCard(
      30, "Flash  (/flash - LittleFS)", info.flashMounted, false,
      info.flashTotalBytes, info.flashUsedBytes, info.flashFreeBytes,
      "Internal LittleFS partition not mounted",
      "Check spiffs partition table");

  // 3. Probe /sd mount state; if mounted, show MOUNTED + "Calculating..." while scanning FAT
  bool sdPresent = lakatosProbeSdMounted(&info, forceProbeSd);
  char sdTitle[48];
  if (sdPresent && info.sdCardType && info.sdCardType[0]) {
    snprintf(sdTitle, sizeof(sdTitle), "SD Card  (/sd - %s)", info.sdCardType);
  } else {
    snprintf(sdTitle, sizeof(sdTitle), "SD Card  (/sd - MicroSD)");
  }

  if (sdPresent) {
    drawStoragePartitionCard(
        134, sdTitle, true, true,
        0, 0, 0,
        "No MicroSD card detected in slot",
        "No card present");
    lakatosGetSdStorageSpace(&info);
  }

  // 4. Update /sd card with final free/used/total space (or NOT MOUNTED)
  drawStoragePartitionCard(
      134, sdTitle, info.sdMounted, false,
      info.sdTotalBytes, info.sdUsedBytes, info.sdFreeBytes,
      "No MicroSD card detected in slot",
      "No card present");
}

void runLakatosStorageScreen() {
  lakatosSetInConfigScreen(true);
  drawStorageScreen(true);
  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    updateLakatosConfigBackground();
    if (lakatosShouldExitApp()) {
      lakatosSetInConfigScreen(false);
      return;
    }
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        if (t.y <= 25 && t.x <= 62) {
          lakatosDevLogf("Button: STORAGE -> HOME (%d,%d)", t.x, t.y);
          lakatosSetInConfigScreen(false);
          return;
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

// ---------------------------------------------------------------------------
// ABOUT Lakatos Operating System Screen
// ---------------------------------------------------------------------------
void runLakatosAboutScreen() {
  lakatosSetInConfigScreen(true);
  {
    LakatosTftGuard tftLock;
    tft.fillScreen(0x0841);
    tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);

    // [ HOME ] (top-left -> goes back to Home page)
    tft.fillRoundRect(2, 2, 58, 20, 4, 0x2945);
    tft.drawRoundRect(2, 2, 58, 20, 4, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x2945);
    tft.drawString("HOME", 31, 12, 2);

    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("ABOUT", 160, 12, 2);

    // Main Card
    tft.fillRoundRect(6, 30, 308, 204, 6, 0x1082);
    tft.drawRoundRect(6, 30, 308, 204, 6, TFT_CYAN);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x1082);
    tft.drawString("Lakatos Operating System", 160, 38, 2);

    tft.setTextColor(TFT_YELLOW, 0x1082);
    tft.drawString("YakFace  |  ESP32-2432S028R (240 MHz)", 160, 58, 1);

    tft.drawFastHLine(16, 72, 288, 0x39E7);

    tft.setTextDatum(TL_DATUM);
    int y = 80;
    auto drawRow = [&](const char* label, const String& val, uint16_t valCol) {
      tft.setTextColor(TFT_LIGHTGREY, 0x1082);
      tft.drawString(label, 16, y, 2);
      tft.setTextColor(valCol, 0x1082);
      tft.drawString(val, 108, y, 2);
      y += 22;
    };

    drawRow("OS / API:", "Lakatos OS v" + String(LAKATOS_API_VERSION), TFT_WHITE);
    char kverBuf[32];
    snprintf(kverBuf, sizeof(kverBuf), "%lld", (long long)lakatosGetKernelVersionSec());
    drawRow("Kernel Ver:", String(kverBuf), TFT_CYAN);
    drawRow("Device ID:", String(gDeviceName) + "/" + String(gSerialNo), TFT_YELLOW);
    drawRow("Airport:", String(gAirportCode) + " (" + String(gAirportCity) + ")", TFT_CYAN);
    drawRow("Free RAM:", String(ESP.getFreeHeap() / 1024) + " KB SRAM", TFT_WHITE);
    drawRow("/net URL:", String(gShUrl).substring(0, 24), TFT_MAGENTA);
  }

  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    updateLakatosConfigBackground();
    if (lakatosShouldExitApp()) {
      lakatosSetInConfigScreen(false);
      return;
    }
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        if (t.y <= 25 && t.x <= 62) {
          lakatosSetInConfigScreen(false);
          return;  // HOME -> back to Home screen
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

// ---------------------------------------------------------------------------
// Kernel Updates Screen (/net/apps/lakatos.krn check & OTA upgrade)
// ---------------------------------------------------------------------------
static void formatUnixSecUtc(int64_t sec, char* outBuf, size_t maxLen) {
  if (sec <= 0) {
    snprintf(outBuf, maxLen, "--");
    return;
  }
  time_t ts = (time_t)sec;
  struct tm tmUtc;
  gmtime_r(&ts, &tmUtc);
  strftime(outBuf, maxLen, "%Y-%m-%d %H:%M:%S UTC", &tmUtc);
}

static bool probeRemoteKernelHeader(LakatosKernelHeader* outHdr, String* outStatus) {
  memset(outHdr, 0, sizeof(*outHdr));
  if (!lakatosWifiIsConnected()) {
    if (outStatus) *outStatus = "Wi-Fi offline";
    return false;
  }
  int32_t got = lakatosFsRead("/net/apps/lakatos.krn", 0, outHdr, sizeof(*outHdr));
  if (got != (int32_t)sizeof(*outHdr)) {
    if (outStatus) *outStatus = "/net/apps/lakatos.krn unreachable";
    return false;
  }
  if (outHdr->magic != LAKATOS_KERNEL_MAGIC || outHdr->kernel_size < 65536) {
    if (outStatus) *outStatus = "Invalid lakatos.krn header";
    return false;
  }
  int64_t localVer = lakatosGetKernelVersionSec();
  if (outHdr->version_sec > localVer) {
    if (outStatus) *outStatus = "NEW UPDATE AVAILABLE!";
  } else if (outHdr->version_sec == localVer) {
    if (outStatus) *outStatus = "Up to date (same version)";
  } else {
    if (outStatus) *outStatus = "Remote version is older";
  }
  return true;
}

static void drawUpdatesProgressBar(const char* stepMsg, int pct, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.fillRoundRect(10, 178, 300, 54, 6, 0x1082);
  tft.drawRoundRect(10, 178, 300, 54, 6, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, 0x1082);
  tft.drawString(stepMsg ? stepMsg : "", 160, 191, 1);

  int bx = 22, by = 203, bw = 276, bh = 16;
  tft.fillRoundRect(bx, by, bw, bh, 4, TFT_BLACK);
  int clamped = constrain(pct, 0, 100);
  int fw = (bw * clamped) / 100;
  if (fw > 0) {
    tft.fillRoundRect(bx, by, fw, bh, 4, color);
  }
  tft.drawRoundRect(bx, by, bw, bh, 4, TFT_WHITE);

  char pbuf[16];
  snprintf(pbuf, sizeof(pbuf), "%d%%", clamped);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(pbuf, 160, by + bh / 2, 1);
}

static bool ensureOta1UpdaterInstalled(LakatosVfsStream* s,
                                       const LakatosKernelHeader& hdr,
                                       const esp_partition_t* ota1Part,
                                       String* outErr) {
  if (!ota1Part) {
    if (outErr) *outErr = "app1 (ota_1) partition missing!";
    return false;
  }
  if (hdr.updater_size == 0) {
    uint8_t magic = 0;
    if (esp_partition_read(ota1Part, 0, &magic, 1) == ESP_OK && magic == 0xE9) {
      return true;
    }
    if (outErr) *outErr = "No updater in ota_1 or lakatos.krn";
    return false;
  }
  if (hdr.updater_size > ota1Part->size) {
    if (outErr) *outErr = "Updater exceeds ota_1 size";
    return false;
  }

  uint8_t* secBuf = (uint8_t*)malloc(4096);
  if (!secBuf) {
    if (outErr) *outErr = "Out of memory for updater buffer";
    return false;
  }

  // Check if ota_1 already contains this exact updater image (by CRC32)
  uint8_t firstByte = 0;
  bool alreadyMatches = false;
  if (esp_partition_read(ota1Part, 0, &firstByte, 1) == ESP_OK && firstByte == 0xE9) {
    uint32_t crc = 0;
    uint32_t rem = hdr.updater_size;
    uint32_t off = 0;
    bool readOk = true;
    while (rem > 0) {
      uint32_t n = (rem > 4096u) ? 4096u : rem;
      if (esp_partition_read(ota1Part, off, secBuf, n) != ESP_OK) {
        readOk = false;
        break;
      }
      crc = crc32_le(crc, secBuf, n);
      off += n;
      rem -= n;
    }
    if (readOk && crc == hdr.updater_crc32) {
      alreadyMatches = true;
    }
  }

  if (alreadyMatches) {
    free(secBuf);
    return true;
  }

  // Stream updater_size bytes from s into ota1Part and verify CRC32
  uint32_t crc = 0;
  uint32_t offset = 0;
  int lastPct = -1;
  while (offset < hdr.updater_size) {
    uint32_t want = hdr.updater_size - offset;
    if (want > 4096u) want = 4096u;
    memset(secBuf, 0xFF, 4096);
    uint32_t got = 0;
    while (got < want) {
      int32_t r = lakatosFsStreamRead(s, secBuf + got, want - got);
      if (r <= 0) {
        free(secBuf);
        if (outErr) *outErr = "Network drop while reading updater";
        return false;
      }
      got += (uint32_t)r;
    }
    crc = crc32_le(crc, secBuf, want);
    if (esp_partition_erase_range(ota1Part, offset, 4096) != ESP_OK ||
        esp_partition_write(ota1Part, offset, secBuf, 4096) != ESP_OK) {
      free(secBuf);
      if (outErr) *outErr = "Flash write to ota_1 failed";
      return false;
    }
    offset += want;
    int pct = (int)((uint64_t)offset * 100ULL / hdr.updater_size);
    if (pct - lastPct >= 5 || pct == 100) {
      lastPct = pct;
      drawUpdatesProgressBar("Provisioning OTA Updater (ota_1)...", pct, TFT_MAGENTA);
    }
  }
  free(secBuf);

  if (crc != hdr.updater_crc32) {
    if (outErr) *outErr = "Updater CRC32 mismatch";
    return false;
  }
  return true;
}

static bool performKernelOtaUpgrade(String* outErr) {
  const esp_partition_t* app0Part = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t* ota1Part = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  const esp_partition_t* appSlotPart = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "app_slot");

  if (!app0Part || !ota1Part) {
    if (outErr) *outErr = "OTA partition table missing ota_1";
    return false;
  }

  drawUpdatesProgressBar("Opening /net/apps/lakatos.krn...", 0, TFT_CYAN);
  LakatosVfsStream* s = lakatosFsOpenRead("/net/apps/lakatos.krn");
  if (!s) {
    if (outErr) *outErr = "Cannot open /net/apps/lakatos.krn";
    return false;
  }

  LakatosKernelHeader hdr;
  if (lakatosFsStreamRead(s, &hdr, sizeof(hdr)) != (int32_t)sizeof(hdr) ||
      hdr.magic != LAKATOS_KERNEL_MAGIC ||
      hdr.kernel_size < 65536 ||
      hdr.kernel_size > app0Part->size) {
    lakatosFsClose(s);
    if (outErr) *outErr = "Invalid lakatos.krn header/size";
    return false;
  }

  uint8_t* buf = (uint8_t*)malloc(4096);
  if (!buf) {
    lakatosFsClose(s);
    if (outErr) *outErr = "Out of heap memory";
    return false;
  }

  bool useSd = lakatosSdIsMounted();
  File sdFile;
  File lfsFile;
  uint32_t slotCap = 0;
  uint8_t* slotSector = nullptr;

  if (useSd) {
    SD.mkdir("/apps");
    SD.remove("/apps/lakatos.krn.stage");
    sdFile = SD.open("/apps/lakatos.krn.stage", FILE_WRITE);
    if (!sdFile || sdFile.write((const uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) {
      if (sdFile) sdFile.close();
      useSd = false;
    }
  }

  if (!useSd) {
    if (!appSlotPart) {
      free(buf);
      lakatosFsClose(s);
      if (outErr) *outErr = "Neither SD nor app_slot available";
      return false;
    }
    slotSector = (uint8_t*)malloc(4096);
    if (!slotSector) {
      free(buf);
      lakatosFsClose(s);
      if (outErr) *outErr = "Out of heap memory";
      return false;
    }
    slotCap = appSlotPart->size - sizeof(LakatosKernelHeader);
    uint32_t needLfs = (hdr.kernel_size > slotCap) ? (hdr.kernel_size - slotCap) : 0;
    LittleFS.remove("/lakatos.krn.stage");
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < needLfs + 16384u) {
      // Temporarily evict /flash/apps/native.bin if needed to make staging room
      LittleFS.remove("/apps/native.bin");
    }
    if (needLfs > 0) {
      lfsFile = LittleFS.open("/lakatos.krn.stage", "w");
      if (!lfsFile) {
        free(slotSector);
        free(buf);
        lakatosFsClose(s);
        if (outErr) *outErr = "Cannot open /flash staging file";
        return false;
      }
    }
  }

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, 0);

  uint32_t done = 0;
  int lastPct = -1;
  if (!useSd) {
    lakatosInvalidateAppSlotCache();
    memset(slotSector, 0xFF, 4096);
    memcpy(slotSector, &hdr, sizeof(hdr));
  }

  while (done < hdr.kernel_size) {
    uint32_t want = hdr.kernel_size - done;
    if (want > 4096u) want = 4096u;
    int32_t r = lakatosFsStreamRead(s, buf, want);
    if (r <= 0) {
      mbedtls_sha256_free(&shaCtx);
      if (sdFile) { sdFile.close(); SD.remove("/apps/lakatos.krn.stage"); }
      if (lfsFile) { lfsFile.close(); LittleFS.remove("/lakatos.krn.stage"); }
      free(slotSector);
      free(buf);
      lakatosFsClose(s);
      if (outErr) *outErr = "Network drop during kernel download";
      return false;
    }

    mbedtls_sha256_update(&shaCtx, buf, (size_t)r);

    if (useSd) {
      if (sdFile.write(buf, (size_t)r) != (size_t)r) {
        mbedtls_sha256_free(&shaCtx);
        sdFile.close();
        SD.remove("/apps/lakatos.krn.stage");
        free(slotSector);
        free(buf);
        lakatosFsClose(s);
        if (outErr) *outErr = "SD write error during staging";
        return false;
      }
    } else {
      for (int32_t i = 0; i < r; i++) {
        uint32_t absPos = done + (uint32_t)i;
        if (absPos < slotCap) {
          uint32_t inSlot = sizeof(LakatosKernelHeader) + absPos;
          uint32_t secOff = inSlot & 4095u;
          slotSector[secOff] = buf[i];
          if (secOff == 4095u || absPos + 1 == slotCap || absPos + 1 == hdr.kernel_size) {
            uint32_t secBase = inSlot & ~4095u;
            if (esp_partition_erase_range(appSlotPart, secBase, 4096) != ESP_OK ||
                esp_partition_write(appSlotPart, secBase, slotSector, 4096) != ESP_OK) {
              mbedtls_sha256_free(&shaCtx);
              if (lfsFile) { lfsFile.close(); LittleFS.remove("/lakatos.krn.stage"); }
              free(slotSector);
              free(buf);
              lakatosFsClose(s);
              if (outErr) *outErr = "app_slot flash write error";
              return false;
            }
            memset(slotSector, 0xFF, 4096);
          }
        } else {
          uint32_t remInBuf = (uint32_t)(r - i);
          if (lfsFile.write(buf + i, remInBuf) != remInBuf) {
            mbedtls_sha256_free(&shaCtx);
            lfsFile.close();
            LittleFS.remove("/lakatos.krn.stage");
            esp_partition_erase_range(appSlotPart, 0, 4096);
            free(slotSector);
            free(buf);
            lakatosFsClose(s);
            if (outErr) *outErr = "/flash write error during staging";
            return false;
          }
          break;
        }
      }
    }

    done += (uint32_t)r;
    int pct = (int)((uint64_t)done * 100ULL / hdr.kernel_size);
    if (pct - lastPct >= 2 || pct == 100) {
      lastPct = pct;
      drawUpdatesProgressBar("Staging /net/apps/lakatos.krn...", pct, TFT_GREEN);
    }
  }

  if (sdFile) sdFile.close();
  if (lfsFile) lfsFile.close();
  free(slotSector);
  free(buf);

  uint8_t digest[32];
  mbedtls_sha256_finish(&shaCtx, digest);
  mbedtls_sha256_free(&shaCtx);

  if (memcmp(digest, hdr.kernel_sha256, 32) != 0) {
    if (useSd) SD.remove("/apps/lakatos.krn.stage");
    else {
      esp_partition_erase_range(appSlotPart, 0, 4096);
      LittleFS.remove("/lakatos.krn.stage");
    }
    lakatosFsClose(s);
    if (outErr) *outErr = "SHA-256 verification failed!";
    return false;
  }

  // Ensure ota_1 has the updater binary before switching boot partition
  if (!ensureOta1UpdaterInstalled(s, hdr, ota1Part, outErr)) {
    lakatosFsClose(s);
    return false;
  }
  lakatosFsClose(s);

  drawUpdatesProgressBar("Verified! Rebooting to OTA Updater...", 100, TFT_YELLOW);
  if (esp_ota_set_boot_partition(ota1Part) != ESP_OK) {
    if (outErr) *outErr = "esp_ota_set_boot_partition failed";
    return false;
  }
  delay(400);
  ESP.restart();
  return true;
}

static void drawUpdatesScreen(bool reachable, const LakatosKernelHeader& remHdr, const String& statusMsg) {
  const int cs = lakatosGetCornerSize();
  const int64_t curVer = lakatosGetKernelVersionSec();
  LakatosTftGuard tftLock;

  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);

  // NW Corner Exit X
  tft.fillRect(0, 0, cs, cs, TFT_BLACK);
  tft.drawLine(0, 0, cs - 1, cs - 1, TFT_WHITE);
  tft.drawLine(cs - 1, 0, 0, cs - 1, TFT_WHITE);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("KERNEL UPDATES", 145, 12, 2);

  // [REFRESH] button on top-right
  tft.fillRoundRect(248, 2, 68, 20, 4, 0x2945);
  tft.drawRoundRect(248, 2, 68, 20, 4, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("REFRESH", 282, 12, 1);

  // Card 1: Running Kernel Version (y: 28..96)
  tft.fillRoundRect(6, 28, 308, 66, 6, 0x1082);
  tft.drawRoundRect(6, 28, 308, 66, 6, 0x39E7);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, 0x1082);
  tft.drawString("CURRENT RUNNING KERNEL:", 14, 34, 1);

  char curBuf[48];
  snprintf(curBuf, sizeof(curBuf), "%lld", (long long)curVer);
  tft.setTextColor(TFT_WHITE, 0x1082);
  tft.drawString(curBuf, 14, 48, 4);

  char curDate[48];
  formatUnixSecUtc(curVer, curDate, sizeof(curDate));
  tft.setTextColor(TFT_LIGHTGREY, 0x1082);
  tft.drawString(curDate, 14, 78, 1);

  // Card 2: Available Kernel at /net/apps/lakatos.krn (y: 100..172)
  tft.fillRoundRect(6, 100, 308, 72, 6, 0x1082);
  uint16_t card2Border = !reachable ? TFT_ORANGE
                       : ((remHdr.version_sec > curVer) ? TFT_GREEN : TFT_CYAN);
  tft.drawRoundRect(6, 100, 308, 72, 6, card2Border);
  tft.setTextColor(TFT_MAGENTA, 0x1082);
  tft.drawString("AVAILABLE (/net/apps/lakatos.krn):", 14, 106, 1);

  if (reachable) {
    char remBuf[48];
    snprintf(remBuf, sizeof(remBuf), "%lld", (long long)remHdr.version_sec);
    uint16_t verCol = (remHdr.version_sec > curVer) ? TFT_GREEN : TFT_YELLOW;
    tft.setTextColor(verCol, 0x1082);
    tft.drawString(remBuf, 14, 120, 4);

    char remDate[64];
    char utcStr[40];
    formatUnixSecUtc(remHdr.version_sec, utcStr, sizeof(utcStr));
    snprintf(remDate, sizeof(remDate), "%s (%u KB)", utcStr, (unsigned)(remHdr.kernel_size / 1024));
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString(remDate, 14, 148, 1);

    tft.setTextColor((remHdr.version_sec > curVer) ? TFT_GREEN : TFT_CYAN, 0x1082);
    tft.drawString(statusMsg, 14, 160, 1);
  } else {
    tft.setTextColor(TFT_ORANGE, 0x1082);
    tft.drawString("UNREACHABLE", 14, 124, 2);
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString(statusMsg, 14, 150, 1);
  }

  // Bottom Action Area (y: 178..232)
  if (reachable) {
    bool isNewer = (remHdr.version_sec > curVer);
    uint16_t btnBg = isNewer ? 0x03E0 : 0x18E3;
    uint16_t btnBorder = isNewer ? TFT_GREEN : TFT_CYAN;
    tft.fillRoundRect(10, 180, 300, 50, 6, btnBg);
    tft.drawRoundRect(10, 180, 300, 50, 6, btnBorder);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, btnBg);
    if (isNewer) {
      tft.drawString("UPGRADE LAKATOS KERNEL NOW", 160, 198, 2);
    } else {
      tft.drawString("REINSTALL / FORCE UPGRADE", 160, 198, 2);
    }
    tft.setTextColor(TFT_YELLOW, btnBg);
    tft.drawString("Stages & verifies SHA-256 before flashing", 160, 217, 1);
  } else {
    tft.fillRoundRect(10, 180, 300, 50, 6, 0x18E3);
    tft.drawRoundRect(10, 180, 300, 50, 6, 0x39E7);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, 0x18E3);
    tft.drawString("Tap REFRESH to check /net/apps/lakatos.krn", 160, 205, 1);
  }
}

void runLakatosUpdatesScreen() {
  lakatosSetInConfigScreen(true);

  LakatosKernelHeader remHdr;
  String statusMsg = "Checking /net/apps/lakatos.krn...";
  drawUpdatesScreen(false, remHdr, statusMsg);

  bool reachable = probeRemoteKernelHeader(&remHdr, &statusMsg);
  drawUpdatesScreen(reachable, remHdr, statusMsg);
  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    updateLakatosConfigBackground();
    if (lakatosShouldExitApp()) {
      lakatosSetInConfigScreen(false);
      return;
    }
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        int cs = lakatosGetCornerSize();
        int hitSz = (cs < 24) ? 24 : cs;
        if (t.x < hitSz && t.y <= hitSz) {
          lakatosSetInConfigScreen(false);
          return;
        } else if (t.y <= 25 && t.x >= 244) {
          statusMsg = "Checking /net/apps/lakatos.krn...";
          drawUpdatesScreen(false, remHdr, statusMsg);
          reachable = probeRemoteKernelHeader(&remHdr, &statusMsg);
          drawUpdatesScreen(reachable, remHdr, statusMsg);
        } else if (reachable && t.y >= 178 && t.y <= 234 && t.x >= 10 && t.x <= 310) {
          String err;
          if (!performKernelOtaUpgrade(&err)) {
            statusMsg = err;
            drawUpdatesScreen(reachable, remHdr, statusMsg);
          }
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

static void drawAutoUpgradeExplanationScreen(const char* appPath, int64_t curVer, int64_t reqVer) {
  LakatosTftGuard tftLock;
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("AUTOMATIC KERNEL UPGRADE", 160, 12, 2);

  tft.fillRoundRect(10, 30, 300, 140, 6, 0x1082);
  tft.drawRoundRect(10, 30, 300, 140, 6, TFT_CYAN);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, 0x1082);
  tft.drawString("App requires a newer Lakatos kernel:", 18, 36, 2);

  char pathBuf[40];
  snprintf(pathBuf, sizeof(pathBuf), "%s", appPath ? appPath : "(app)");
  tft.setTextColor(TFT_CYAN, 0x1082);
  tft.drawString(pathBuf, 18, 56, 2);

  char tsBuf[32];
  char lineBuf[52];
  formatUnixSecUtc(curVer, tsBuf, sizeof(tsBuf));
  snprintf(lineBuf, sizeof(lineBuf), "Running:  %s", tsBuf);
  tft.setTextColor(TFT_ORANGE, 0x1082);
  tft.drawString(lineBuf, 18, 80, 1);

  formatUnixSecUtc(reqVer, tsBuf, sizeof(tsBuf));
  snprintf(lineBuf, sizeof(lineBuf), "Required: %s", tsBuf);
  tft.setTextColor(TFT_YELLOW, 0x1082);
  tft.drawString(lineBuf, 18, 93, 1);

  tft.setTextColor(TFT_GREEN, 0x1082);
  tft.drawString("Automatically loading /net/apps/lakatos.krn", 18, 112, 1);
  tft.drawString("and upgrading OS (please wait)...", 18, 125, 1);

  tft.setTextColor(TFT_SILVER, 0x1082);
  tft.drawString("Device will reboot when upgrade completes.", 18, 146, 1);
}

static void drawAutoUpgradeFailureScreen(const char* appPath,
                                         int64_t reqVer,
                                         bool reachable,
                                         const LakatosKernelHeader& remHdr,
                                         const String& failReason) {
  LakatosTftGuard tftLock;
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_MAROON);
  lakatosDrawExitWedge();

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_MAROON);
  tft.drawString("KERNEL UPGRADE FAILED", 168, 12, 2);

  tft.fillRoundRect(10, 30, 300, 156, 6, 0x1082);
  tft.drawRoundRect(10, 30, 300, 156, 6, TFT_RED);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, 0x1082);
  tft.drawString("Could not upgrade kernel for app:", 18, 36, 2);

  char pathBuf[40];
  snprintf(pathBuf, sizeof(pathBuf), "%s", appPath ? appPath : "(app)");
  tft.setTextColor(TFT_CYAN, 0x1082);
  tft.drawString(pathBuf, 18, 56, 2);

  tft.setTextColor(TFT_SILVER, 0x1082);
  tft.drawString("Automatic upgrade from /net/apps/lakatos.krn", 18, 80, 1);
  tft.drawString("failed with error:", 18, 92, 1);

  tft.fillRoundRect(16, 106, 288, 34, 4, 0x2000);
  tft.drawRoundRect(16, 106, 288, 34, 4, TFT_ORANGE);

  const char* errStr = failReason.c_str();
  size_t errLen = strlen(errStr);
  char errLine1[48];
  char errLine2[48];
  if (errLen <= 45) {
    snprintf(errLine1, sizeof(errLine1), "%s", errStr);
    errLine2[0] = '\0';
  } else {
    int split = 45;
    while (split > 20 && errStr[split] != ' ') split--;
    memcpy(errLine1, errStr, (size_t)split);
    errLine1[split] = '\0';
    const char* rest = errStr + split;
    while (*rest == ' ') rest++;
    snprintf(errLine2, sizeof(errLine2), "%s", rest);
    errLine2[45] = '\0';
  }
  tft.setTextColor(TFT_YELLOW, 0x2000);
  tft.drawString(errLine1, 22, (errLine2[0] != '\0') ? 112 : 119, 1);
  if (errLine2[0] != '\0') {
    tft.drawString(errLine2, 22, 125, 1);
  }

  tft.setTextColor(TFT_LIGHTGREY, 0x1082);
  if (reachable) {
    char tsBuf[32];
    char lineBuf[52];
    formatUnixSecUtc(remHdr.version_sec, tsBuf, sizeof(tsBuf));
    snprintf(lineBuf, sizeof(lineBuf), "Available: %s", tsBuf);
    tft.drawString(lineBuf, 18, 147, 1);
    formatUnixSecUtc(reqVer, tsBuf, sizeof(tsBuf));
    snprintf(lineBuf, sizeof(lineBuf), "Required:  %s", tsBuf);
    tft.drawString(lineBuf, 18, 160, 1);
  } else {
    tft.drawString("Check Wi-Fi connection and ensure a current", 18, 147, 1);
    tft.drawString("lakatos.krn is pushed to /net/apps/.", 18, 160, 1);
  }

  tft.fillRoundRect(20, 194, 134, 34, 5, 0x2124);
  tft.drawRoundRect(20, 194, 134, 34, 5, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, 0x2124);
  tft.drawString("OPEN UPDATES", 87, 211, 2);

  tft.fillRoundRect(166, 194, 134, 34, 5, 0x3186);
  tft.drawRoundRect(166, 194, 134, 34, 5, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x3186);
  tft.drawString("OK", 233, 211, 2);
  tft.setTextDatum(TL_DATUM);
}

void lakatosAutoUpgradeKernelForApp(const char* appPath, int64_t reqVer) {
  int64_t curVer = lakatosGetKernelVersionSec();
  Serial.printf("[Lakatos] App '%s' requires kernel %lld (running %lld) -> attempting automatic OTA upgrade...\r\n",
                appPath ? appPath : "", (long long)reqVer, (long long)curVer);

  lakatosSetRgbLed(false, false, true, 150);
  drawAutoUpgradeExplanationScreen(appPath, curVer, reqVer);
  drawUpdatesProgressBar("Checking /net/apps/lakatos.krn...", 0, TFT_CYAN);

  LakatosKernelHeader remHdr;
  String probeStatus;
  bool reachable = probeRemoteKernelHeader(&remHdr, &probeStatus);

  String failReason;
  if (!reachable) {
    failReason = probeStatus.length() > 0 ? probeStatus : "/net/apps/lakatos.krn unreachable";
  } else if (remHdr.version_sec <= curVer) {
    failReason = "/net/apps/lakatos.krn is not newer than running OS";
  } else if (remHdr.version_sec < reqVer) {
    failReason = "/net/apps/lakatos.krn is older than required by app";
  } else {
    if (performKernelOtaUpgrade(&failReason)) {
      return;
    }
    if (failReason.length() == 0) {
      failReason = "OTA upgrade failed";
    }
  }

  Serial.printf("[Lakatos] Automatic kernel upgrade for '%s' failed: %s\r\n",
                appPath ? appPath : "", failReason.c_str());
  lakatosSetRgbLed(true, false, false, 400);
  lakatosSetInConfigScreen(true);
  drawAutoUpgradeFailureScreen(appPath, reqVer, reachable, remHdr, failReason);
  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    updateLakatosConfigBackground();
    lakatosUpdateRgbLed();
    if (lakatosShouldExitApp()) {
      lakatosSetInConfigScreen(false);
      return;
    }
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        int cs = lakatosGetCornerSize();
        int hitSz = (cs < 24) ? 24 : cs;
        if (t.x < hitSz && t.y <= hitSz) {
          lakatosWaitTouchRelease();
          lakatosSetInConfigScreen(false);
          return;
        }
        if (t.y >= 190 && t.y <= 232) {
          if (t.x >= 18 && t.x <= 156) {
            lakatosWaitTouchRelease();
            lakatosSetInConfigScreen(false);
            runLakatosUpdatesScreen();
            return;
          }
          if (t.x >= 164 && t.x <= 302) {
            lakatosWaitTouchRelease();
            lakatosSetInConfigScreen(false);
            return;
          }
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

// ---------------------------------------------------------------------------
// Main CONFIG Screen
// ---------------------------------------------------------------------------
static void drawConfigScreen() {
  const bool isDev = gTouchSpotEnabled;
  const int  cs    = lakatosGetCornerSize();
  LakatosTftGuard tftLock;
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);

  // Top-left NW Exit X (changes size with gCornerSize)
  tft.fillRect(0, 0, cs, cs, TFT_BLACK);
  tft.drawLine(0, 0, cs - 1, cs - 1, TFT_WHITE);
  tft.drawLine(cs - 1, 0, 0, cs - 1, TFT_WHITE);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("CONFIG", 160, 12, 2);

  // Top-right NE Corner Preview Square (shows how big the corner target is)
  tft.fillRect(LAKATOS_SCREEN_W - cs, 0, cs, cs, TFT_BLACK);
  tft.drawRect(LAKATOS_SCREEN_W - cs, 0, cs, cs, TFT_WHITE);

  // Row 1 (y: 26..56): Brightness Slider + [AUTO] toggle
  tft.fillRoundRect(4, 26, 312, 30, 5, 0x1082);
  tft.drawRoundRect(4, 26, 312, 30, 5, 0x39E7);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x1082);
  tft.drawString("BRIGHT:", 10, 41, 1);

  // Brightness slider track (x: 58..232, y: 35..47)
  int trackX = 58, trackW = 174;
  tft.fillRoundRect(trackX, 35, trackW, 12, 4, TFT_BLACK);
  int fillW = (trackW * gCurrentBrightPct) / 100;
  tft.fillRoundRect(trackX, 35, fillW, 12, 4, gAutoBright ? TFT_CYAN : TFT_ORANGE);
  tft.drawRoundRect(trackX, 35, trackW, 12, 4, TFT_WHITE);

  // [x] Auto checkbox (x: 240..310, y: 29..53)
  uint16_t autoBg = gAutoBright ? 0x1A26 : 0x18E3;
  uint16_t autoBorder = gAutoBright ? TFT_GREEN : TFT_LIGHTGREY;
  tft.fillRoundRect(240, 29, 70, 24, 4, autoBg);
  tft.drawRoundRect(240, 29, 70, 24, 4, autoBorder);
  // Checkbox square at (246, 34, 14, 14)
  tft.fillRect(246, 34, 14, 14, gAutoBright ? TFT_DARKGREEN : TFT_BLACK);
  tft.drawRect(246, 34, 14, 14, TFT_WHITE);
  if (gAutoBright) {
    tft.drawLine(248, 41, 251, 45, TFT_YELLOW);
    tft.drawLine(251, 45, 257, 36, TFT_YELLOW);
    tft.drawLine(248, 42, 251, 46, TFT_YELLOW);
    tft.drawLine(251, 46, 257, 37, TFT_YELLOW);
  }
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(gAutoBright ? TFT_YELLOW : TFT_LIGHTGREY, autoBg);
  tft.drawString("Auto", 265, 41, 1);

  // Row 2 (y: 59..89): Corner Size Slider with stops at 8, 12, 16, 20, 24
  tft.fillRoundRect(4, 59, 312, 30, 5, 0x1082);
  tft.drawRoundRect(4, 59, 312, 30, 5, 0x39E7);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x1082);
  tft.drawString("CORNER:", 10, 74, 1);

  static const int kStops[5] = {8, 12, 16, 20, 24};
  const int cTrackX0 = 68;
  const int cStepX   = 40;  // Stops at x = 68, 108, 148, 188, 228
  const int cTrackW  = cStepX * 4;
  tft.fillRoundRect(cTrackX0, 66, cTrackW, 6, 3, TFT_BLACK);
  int activeStopIdx = (cs - 8) / 4;
  if (activeStopIdx < 0) activeStopIdx = 0;
  if (activeStopIdx > 4) activeStopIdx = 4;
  if (activeStopIdx > 0) {
    tft.fillRoundRect(cTrackX0, 66, activeStopIdx * cStepX, 6, 3, TFT_CYAN);
  }
  tft.drawRoundRect(cTrackX0, 66, cTrackW, 6, 3, TFT_WHITE);

  tft.setTextDatum(TC_DATUM);
  for (int i = 0; i < 5; i++) {
    int sx = cTrackX0 + i * cStepX;
    bool sel = (kStops[i] == cs);
    tft.drawFastVLine(sx, 63, 12, sel ? TFT_YELLOW : TFT_LIGHTGREY);
    if (sel) {
      tft.fillCircle(sx, 69, 5, TFT_YELLOW);
      tft.drawCircle(sx, 69, 5, TFT_WHITE);
    } else {
      tft.fillCircle(sx, 69, 3, 0x2945);
      tft.drawCircle(sx, 69, 3, TFT_CYAN);
    }
    char sbuf[8];
    snprintf(sbuf, sizeof(sbuf), "%d", kStops[i]);
    tft.setTextColor(sel ? TFT_YELLOW : TFT_LIGHTGREY, 0x1082);
    tft.drawString(sbuf, sx, 77, 1);
  }

  // Value badge on right of Corner Size row (x: 246..310, y: 62..86)
  tft.fillRoundRect(246, 62, 64, 24, 4, 0x18E3);
  tft.drawRoundRect(246, 62, 64, 24, 4, TFT_CYAN);
  char cornerLbl[16];
  snprintf(cornerLbl, sizeof(cornerLbl), "%d px", cs);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x18E3);
  tft.drawString(cornerLbl, 278, 74, 2);

  // Row 3 (y: 92..133): [DEV NAME] (x: 4..157) & [SERIAL #] (x: 163..316)
  tft.fillRoundRect(4, 92, 153, 41, 5, 0x18E3);
  tft.drawRoundRect(4, 92, 153, 41, 5, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, 0x18E3);
  tft.drawString("DEVICE NAME:", 10, 96, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString(gDeviceName, 10, 109, 2);

  uint16_t serBg = isDev ? 0x18E3 : 0x0841;
  if (isDev) {
    tft.fillRoundRect(163, 92, 153, 41, 5, serBg);
    tft.drawRoundRect(163, 92, 153, 41, 5, TFT_CYAN);
  }
  tft.setTextColor(isDev ? TFT_CYAN : TFT_LIGHTGREY, serBg);
  tft.drawString("SERIAL NUMBER:", 169, 96, 1);
  tft.setTextColor(TFT_WHITE, serBg);
  tft.drawString(gSerialNo, 169, 109, 2);

  // Row 4 (y: 136..177): [/net URL] (x: 4..198) & [/net PASS] (x: 204..316)
  tft.fillRoundRect(4, 136, 194, 41, 5, 0x18E3);
  tft.drawRoundRect(4, 136, 194, 41, 5, TFT_MAGENTA);
  tft.setTextColor(TFT_MAGENTA, 0x18E3);
  tft.drawString("/net SERVER URL:", 10, 140, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  String shortUrl = String(gShUrl);
  if (shortUrl.length() > 28) shortUrl = shortUrl.substring(0, 28);
  tft.drawString(shortUrl, 10, 155, 1);

  tft.fillRoundRect(204, 136, 112, 41, 5, 0x18E3);
  tft.drawRoundRect(204, 136, 112, 41, 5, TFT_MAGENTA);
  tft.setTextColor(TFT_MAGENTA, 0x18E3);
  tft.drawString("/net PASSWORD:", 210, 140, 1);
  tft.setTextColor(gShPass[0] ? TFT_GREEN : TFT_ORANGE, 0x18E3);
  String shortPass = gShPass[0] ? String(gShPass) : String("(NOT SET)");
  if (shortPass.length() > 17) shortPass = shortPass.substring(0, 17);
  tft.drawString(shortPass, 210, 155, 1);

  // Row 5 (y: 180..236): [AIRPORT] (x: 4..104), [WI-FI] (x: 109..210), [DEV/CALIBRATE] (x: 215..316)
  tft.fillRoundRect(4, 180, 100, 56, 6, 0x18E3);
  tft.drawRoundRect(4, 180, 100, 56, 6, TFT_YELLOW);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x18E3);
  tft.drawString("AIRPORT", 54, 192, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString(gAirportCode, 54, 215, 4);

  tft.fillRoundRect(109, 180, 101, 56, 6, 0x18E3);
  tft.drawRoundRect(109, 180, 101, 56, 6, TFT_GREEN);
  tft.setTextColor(TFT_GREEN, 0x18E3);
  tft.drawString("WI-FI SETUP", 159, 195, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString((WiFi.status() == WL_CONNECTED) ? "ONLINE" : "CONFIGURE", 159, 216, 2);

  // Right column (x: 215..316): [x] DEVELOPER checkbox (y: 180..206) + [CALIBRATE] (y: 210..236)
  if (gCanBeDeveloper || isDev) {
    uint16_t spotBg = isDev ? 0x1A26 : 0x18E3;
    uint16_t spotBorder = isDev ? TFT_GREEN : TFT_LIGHTGREY;
    tft.fillRoundRect(215, 180, 101, 26, 5, spotBg);
    tft.drawRoundRect(215, 180, 101, 26, 5, spotBorder);
    // Checkbox square at (221, 186, 14, 14)
    tft.fillRect(221, 186, 14, 14, isDev ? TFT_DARKGREEN : TFT_BLACK);
    tft.drawRect(221, 186, 14, 14, TFT_WHITE);
    if (isDev) {
      // Draw checkmark
      tft.drawLine(223, 193, 226, 197, TFT_YELLOW);
      tft.drawLine(226, 197, 232, 188, TFT_YELLOW);
      tft.drawLine(223, 194, 226, 198, TFT_YELLOW);
      tft.drawLine(226, 198, 232, 189, TFT_YELLOW);
    }
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(isDev ? TFT_YELLOW : TFT_LIGHTGREY, spotBg);
    tft.drawString("DEVELOPER", 240, 193, 1);
  }

  tft.fillRoundRect(215, 210, 101, 26, 5, 0x18E3);
  tft.drawRoundRect(215, 210, 101, 26, 5, TFT_ORANGE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_ORANGE, 0x18E3);
  tft.drawString("CALIBRATE", 265, 223, 2);
}

void runLakatosConfigScreen(bool startInCalibration) {
  lakatosSetInConfigScreen(true);
  int secretTitleTaps = 0;

  if (startInCalibration) {
    runTouchCalibrationModal();
    lakatosSetInConfigScreen(false);
    return;
  }
  drawConfigScreen();
  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastRel = millis();

  while (true) {
    updateLakatosConfigBackground();
    if (lakatosShouldExitApp()) {
      lakatosSetInConfigScreen(false);
      return;
    }
    if (lakatosConsumeConfigRedraw()) {
      drawConfigScreen();
    }
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();

    if (t.touched) {
      // Allow live dragging on the brightness slider (Row 1: y = 26..56, x = 54..236)
      if (t.y >= 26 && t.y <= 56 && t.x >= 54 && t.x <= 236) {
        gAutoBright = false;
        int pct = ((t.x - 58) * 100) / 174;
        gManualBrightPct = constrain(pct, 5, 100);
        applyBacklightPercent(gManualBrightPct);
        saveConfigToNvs();
        drawConfigScreen();
      } else if (t.y >= 59 && t.y <= 89 && t.x >= 54 && t.x <= 242) {
        // Allow live dragging/tapping on the 5-stop Corner Size slider (8, 12, 16, 20, 24)
        int stopIdx = (t.x - 68 + 20) / 40;
        stopIdx = constrain(stopIdx, 0, 4);
        int nextSz = 8 + stopIdx * 4;
        if (nextSz != gCornerSize) {
          lakatosSetCornerSize(nextSz);
          lakatosConsumeConfigRedraw();
          lakatosDevLogf("Slider: CONFIG -> CORNER SIZE=%d (%d,%d)", gCornerSize, t.x, t.y);
          drawConfigScreen();
        }
      } else if (!wasDown && (now - lastRel > 50)) {
        wasDown = true;
        int cs = lakatosGetCornerSize();
        int hitSz = (cs < 24) ? 24 : cs;
        if (t.x < hitSz && t.y <= hitSz) {
          lakatosDevLogf("Button: CONFIG -> EXIT X (%d,%d)", t.x, t.y);
          lakatosSetInConfigScreen(false);
          return;  // Top-left X -> Exit Config back to YakFace Home page
        } else if (t.y <= 24 && t.x >= 90 && t.x <= 230) {
          secretTitleTaps++;
          if (secretTitleTaps >= 8 && !gCanBeDeveloper) {
            gCanBeDeveloper = true;
            drawConfigScreen();
          }
        } else if (t.y >= 26 && t.y <= 56 && t.x >= 238) {
          gAutoBright = !gAutoBright;
          lakatosDevLogf("Button: CONFIG -> AUTO BRIGHT=%s (%d,%d)", gAutoBright ? "ON" : "OFF", t.x, t.y);
          if (gAutoBright) {
            applyBacklightPercent(computeAutoBrightPct(gSmoothedLdr));
          } else {
            applyBacklightPercent(gManualBrightPct);
          }
          saveConfigToNvs();
          drawConfigScreen();
        } else if (t.y >= 59 && t.y <= 89 && t.x >= 244) {
          // Tapping the "XX px" badge on the right cycles stops 8 -> 12 -> 16 -> 20 -> 24 -> 8
          int nextSz = gCornerSize + 4;
          if (nextSz > 24) nextSz = 8;
          lakatosSetCornerSize(nextSz);
          lakatosConsumeConfigRedraw();
          lakatosDevLogf("Button: CONFIG -> CORNER SIZE=%d (%d,%d)", gCornerSize, t.x, t.y);
          drawConfigScreen();
        } else if (t.y >= 92 && t.y <= 133) {
          if (t.x < 160) {
            lakatosDevLogf("Button: CONFIG -> DEVICE NAME (%d,%d)", t.x, t.y);
            if (lakatosPromptKeyboard("DEVICE NAME", gDeviceName, sizeof(gDeviceName), false) == 1) {
              saveConfigToNvs();
            }
            drawConfigScreen();
          } else if (lakatosGetDeveloperMode()) {
            lakatosDevLogf("Button: CONFIG -> SERIAL NUMBER (%d,%d)", t.x, t.y);
            if (lakatosPromptKeyboard("SERIAL NUMBER", gSerialNo, sizeof(gSerialNo), false) == 1) {
              saveConfigToNvs();
            }
            drawConfigScreen();
          }
        } else if (t.y >= 136 && t.y <= 177) {
          if (t.x < 200) {
            lakatosDevLogf("Button: CONFIG -> /net URL (%d,%d)", t.x, t.y);
            if (lakatosPromptKeyboard("SUPERHIGHWAY URL", gShUrl, sizeof(gShUrl), false) == 1) {
              saveConfigToNvs();
            }
          } else {
            lakatosDevLogf("Button: CONFIG -> /net PASSWORD (%d,%d)", t.x, t.y);
            char passBuf[sizeof(gShPass)] = "";
            if (lakatosPromptKeyboard("/net PASSWORD", passBuf, sizeof(passBuf), false) == 1) {
              strncpy(gShPass, passBuf, sizeof(gShPass) - 1);
              gShPass[sizeof(gShPass) - 1] = '\0';
              saveConfigToNvs();
            }
          }
          drawConfigScreen();
        } else if (t.y >= 180) {
          if (t.x <= 106) {
            lakatosDevLogf("Button: CONFIG -> AIRPORT (%d,%d)", t.x, t.y);
            if (runAirportSelectorModal()) {
              lakatosSetInConfigScreen(false);
              return;
            }
            drawConfigScreen();
          } else if (t.x <= 212) {
            lakatosDevLogf("Button: CONFIG -> WI-FI SETUP (%d,%d)", t.x, t.y);
            if (runWifiConfigModal()) {
              lakatosSetInConfigScreen(false);
              return;
            }
            drawConfigScreen();
          } else if (t.y < 208) {
            if (gCanBeDeveloper || lakatosGetDeveloperMode()) {
              bool nextDev = !lakatosGetDeveloperMode();
              gCanBeDeveloper = true;
              lakatosSetDeveloperMode(nextDev);
              Serial.printf("[DEV] Developer Mode: %s\r\n", nextDev ? "ON" : "OFF");
              drawConfigScreen();
            }
          } else {
            lakatosDevLogf("Button: CONFIG -> CALIBRATE (%d,%d)", t.x, t.y);
            runTouchCalibrationModal();
            drawConfigScreen();
          }
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

// ---------------------------------------------------------------------------
// Activity Mode Screen (1. Ambient, 2. Demo, 3. Active)
// ---------------------------------------------------------------------------
static void drawModeScreen() {
  const int cs = lakatosGetCornerSize();
  LakatosTftGuard tftLock;
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);

  // Top-left NW Exit X
  tft.fillRect(0, 0, cs, cs, TFT_BLACK);
  tft.drawLine(0, 0, cs - 1, cs - 1, TFT_WHITE);
  tft.drawLine(cs - 1, 0, 0, cs - 1, TFT_WHITE);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("ACTIVITY MODE", 160, 12, 2);

  // Row 1 (y: 28..62): 3 Mode Selector Buttons
  const char* const modeLabels[3] = {"1. AMBIENT", "2. DEMO", "3. ACTIVE"};
  for (int i = 0; i < 3; i++) {
    int modeId = i + 1;
    int bx     = 6 + i * 104;
    bool sel   = (gActivityMode == modeId);
    uint16_t bg     = sel ? 0x1A26 : 0x18E3;
    uint16_t border = sel ? TFT_GREEN : 0x39E7;
    tft.fillRoundRect(bx, 28, 100, 34, 5, bg);
    tft.drawRoundRect(bx, 28, 100, 34, 5, border);
    if (sel) {
      tft.drawRoundRect(bx + 1, 29, 98, 32, 4, TFT_YELLOW);
    }
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(sel ? TFT_YELLOW : TFT_WHITE, bg);
    tft.drawString(modeLabels[i], bx + 50, 45, 2);
  }

  // Row 2 (y: 66..108): Ambient App Name (left) & Ambient Seconds/Image (right)
  tft.fillRoundRect(6, 66, 190, 42, 5, 0x18E3);
  tft.drawRoundRect(6, 66, 190, 42, 5, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, 0x18E3);
  tft.drawString("AMBIENT APP:", 12, 70, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString(gAmbientApp, 12, 84, 2);

  tft.fillRoundRect(202, 66, 112, 42, 5, 0x18E3);
  tft.drawRoundRect(202, 66, 112, 42, 5, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, 0x18E3);
  tft.drawString("SEC / IMAGE:", 208, 70, 1);
  char ambSecBuf[24];
  snprintf(ambSecBuf, sizeof(ambSecBuf), "%d s", gAmbientSec);
  tft.setTextColor(TFT_YELLOW, 0x18E3);
  tft.drawString(ambSecBuf, 208, 84, 2);

  // Row 3 (y: 112..154): Demo Seconds per Run
  tft.fillRoundRect(6, 112, 308, 42, 5, 0x18E3);
  tft.drawRoundRect(6, 112, 308, 42, 5, TFT_MAGENTA);
  tft.setTextColor(TFT_MAGENTA, 0x18E3);
  tft.drawString("DEMO SECONDS PER RUN (DEFAULT 60):", 12, 116, 1);
  char demoBuf[48];
  snprintf(demoBuf, sizeof(demoBuf), "%d s  (Demos + 5 random photos)", gDemoSec);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString(demoBuf, 12, 130, 2);

  // Row 4 (y: 158..200): Active Inactivity Seconds -> Ambient
  tft.fillRoundRect(6, 158, 308, 42, 5, 0x18E3);
  tft.drawRoundRect(6, 158, 308, 42, 5, TFT_ORANGE);
  tft.setTextColor(TFT_ORANGE, 0x18E3);
  tft.drawString("ACTIVE INACTIVITY -> AMBIENT (SEC):", 12, 162, 1);
  char idleBuf[48];
  if (gActiveIdleSec > 0) {
    snprintf(idleBuf, sizeof(idleBuf), "%d s idle on Home -> Ambient", gActiveIdleSec);
  } else {
    snprintf(idleBuf, sizeof(idleBuf), "0 s (Auto-Ambient disabled)");
  }
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString(idleBuf, 12, 176, 2);

  // Row 5 (y: 205..236): Action button to immediately activate chosen mode
  tft.fillRoundRect(6, 205, 308, 31, 5, 0x1A26);
  tft.drawRoundRect(6, 205, 308, 31, 5, TFT_GREEN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, 0x1A26);
  if (gActivityMode == LAKATOS_MODE_AMBIENT) {
    tft.drawString("START AMBIENT MODE NOW", 160, 220, 2);
  } else if (gActivityMode == LAKATOS_MODE_DEMO) {
    tft.drawString("START DEMO MODE NOW", 160, 220, 2);
  } else {
    tft.drawString("RETURN TO ACTIVE HOME", 160, 220, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void runLakatosModeScreen() {
  lakatosSetInConfigScreen(true);
  drawModeScreen();
  lakatosWaitTouchRelease();

  bool wasDown = false;
  uint32_t lastRel = millis();

  while (true) {
    updateLakatosConfigBackground();
    if (lakatosShouldExitApp()) {
      lakatosSetInConfigScreen(false);
      return;
    }

    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        int cs = lakatosGetCornerSize();
        int hitSz = (cs < 24) ? 24 : cs;
        if (t.x < hitSz && t.y <= hitSz) {
          lakatosSetInConfigScreen(false);
          return;
        } else if (t.y >= 28 && t.y <= 62) {
          if (t.x >= 6 && t.x <= 106) {
            lakatosSetActivityMode(LAKATOS_MODE_AMBIENT);
          } else if (t.x >= 110 && t.x <= 210) {
            lakatosSetActivityMode(LAKATOS_MODE_DEMO);
          } else if (t.x >= 214 && t.x <= 314) {
            lakatosSetActivityMode(LAKATOS_MODE_ACTIVE);
          }
          drawModeScreen();
        } else if (t.y >= 66 && t.y <= 108) {
          if (t.x < 198) {
            char buf[sizeof(gAmbientApp)];
            strncpy(buf, gAmbientApp, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if (lakatosPromptKeyboard("AMBIENT APP NAME", buf, sizeof(buf), false) == 1) {
              lakatosSetAmbientApp(buf);
            }
          } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", gAmbientSec);
            if (lakatosPromptKeyboard("AMBIENT SEC / IMAGE", buf, sizeof(buf), false) == 1) {
              int v = atoi(buf);
              if (v > 0) lakatosSetAmbientSec(v);
            }
          }
          drawModeScreen();
        } else if (t.y >= 112 && t.y <= 154) {
          char buf[16];
          snprintf(buf, sizeof(buf), "%d", gDemoSec);
          if (lakatosPromptKeyboard("DEMO SECONDS PER RUN", buf, sizeof(buf), false) == 1) {
            int v = atoi(buf);
            if (v > 0) lakatosSetDemoSec(v);
          }
          drawModeScreen();
        } else if (t.y >= 158 && t.y <= 200) {
          char buf[16];
          snprintf(buf, sizeof(buf), "%d", gActiveIdleSec);
          if (lakatosPromptKeyboard("IDLE SEC TO AMBIENT (0=OFF)", buf, sizeof(buf), false) == 1) {
            int v = atoi(buf);
            if (v >= 0) lakatosSetActiveIdleSec(v);
          }
          drawModeScreen();
        } else if (t.y >= 204 && t.y <= 238 && t.x >= 6 && t.x <= 314) {
          lakatosSetInConfigScreen(false);
          return;
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}
