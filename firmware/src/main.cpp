#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Esp32WifiFix.h>
#include <math.h>
#include <time.h>
#include <string.h>

#include "secrets.h"
#include "certs.h"

// ---- Display size ----
// Most common size is 16x2. If you have a 20x4 display, just change these
// two - row text is short and left-aligned either way, so it just leaves
// more blank space on a wider line rather than needing layout changes.
#define LCD_COLS 16
#define LCD_ROWS 2

// Wiring: SDA -> GPIO4, SCL -> GPIO5
#define PIN_SDA 4
#define PIN_SCL 5

// This calls the real Anthropic API every poll, so don't hammer it. 120s
// matches claude-usage-stick's own default (tested range: 30s-300s).
#define REFRESH_INTERVAL_MS 120000UL
#define HTTP_TIMEOUT_MS 15000UL
#define STALE_AFTER_MS (3UL * REFRESH_INTERVAL_MS)  // flag data as stale after a few missed polls

// The two screens (5-hour detail, weekly detail) alternate on this interval.
#define SCREEN_ROTATE_MS 4000UL

// Same endpoint/headers/probe model Claude Code itself uses when checking
// rate limits - see https://github.com/oauramos/claude-usage-stick.
#define MESSAGES_ENDPOINT "https://api.anthropic.com/v1/messages"
#define ANTHROPIC_VERSION "2023-06-01"
#define PROBE_MODEL "claude-haiku-4-5-20251001"

// A blocking `while (WiFi.status() != WL_CONNECTED) {}` with no timeout
// hangs forever (silently, with no Serial output) if the AP is briefly
// unreachable or misconfigured. Cap each attempt and retry with backoff
// instead of blocking forever.
#define WIFI_CONNECT_TIMEOUT_MS 20000UL
#define WIFI_RETRY_BACKOFF_START_MS 5000UL
#define WIFI_RETRY_BACKOFF_MAX_MS 60000UL

LiquidCrystal_I2C *lcd = nullptr;
Esp32WifiFix wifiFix;

int sessionUtilization = -1;  // 5-hour usage %, -1 = no data yet
int weeklyUtilization = -1;   // 7-day (week) usage %, -1 = no data yet
time_t sessionResetEpoch = 0; // unix time the 5h window resets, 0 = unknown
time_t weeklyResetEpoch = 0;  // unix time the 7d window resets, 0 = unknown
bool dataStale = true;
bool authFailed = false;
unsigned long lastContactMs = 0;

// The display cycles between a 5-hour detail screen and a weekly detail
// screen instead of showing both at once - see readai.md.
int currentScreen = 0;  // 0 = 5H detail, 1 = WK detail
unsigned long lastScreenSwitchMs = 0;

// Tracks what's currently on screen so we only touch the I2C bus when the
// text actually changes.
char renderedRow0[LCD_COLS + 1] = "";
char renderedRow1[LCD_COLS + 1] = "";
bool renderedAuthFailed = false;

// Short (<=9 char) names on purpose - these get printed on a 16-col LCD
// with no truncation, so every state stays fully readable there.
const char *wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCANNED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONN_FAIL";
    case WL_CONNECTION_LOST: return "CONN_LOST";
    case WL_DISCONNECTED: return "DISCONN";
    default: return "UNKNOWN";
  }
}

// WL_DISCONNECTED alone doesn't say why. The 802.11 disconnect reason
// distinguishes a wrong password (fails late, at the 4-way handshake) from
// e.g. reason 2 / AUTH_EXPIRE (fails at the earlier open-system-auth stage -
// never a password problem, more often a router-side issue such as a
// temporary anti-flood MAC block). Stored (not just logged) so the LCD can
// show it too - useful for testing away from a machine with Serial attached.
volatile int lastDisconnectReason = -1;
int wifiFailCount = 0;

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("[wifi] disconnected, reason=%d\n", lastDisconnectReason);
  }
}

unsigned long wifiRetryBackoffMs = WIFI_RETRY_BACKOFF_START_MS;

// Clears a row and writes text left-aligned - avoids leftover characters
// from a previous, longer string when live-updating a diagnostic line.
void lcdPrintRow(int row, const char *text) {
  lcd->setCursor(0, row);
  for (int i = 0; i < LCD_COLS; i++) lcd->print(' ');
  lcd->setCursor(0, row);
  lcd->print(text);
}

uint8_t scanForLcdAddress() {
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      return addr;
    }
  }
  return 0x27;  // common PCF8574 default, used as a fallback if the scan finds nothing
}

// Parses the "2026-07-22T01:00:00Z"-style UTC timestamps Anthropic's
// rate-limit reset headers use. Returns 0 (treated as "unknown", rendered
// as "--") if the string is empty or doesn't parse - check the Serial log
// if a screen ever shows "--" instead of a real countdown, since that's
// the actual header text this parses.
//
// mktime() normally interprets struct tm as *local* time, which would be
// wrong for a UTC string - but configTime() in setup() is called with a
// 0 gmtOffset, making the device's "local" time UTC, so mktime() ends up
// UTC here too. (esp32-arduino's newlib doesn't provide timegm().)
time_t parseIso8601Utc(const char *s) {
  if (!s || !*s) return 0;
  struct tm tmv = {};
  int y, mo, d, h, mi, se;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  tmv.tm_year = y - 1900;
  tmv.tm_mon = mo - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = h;
  tmv.tm_min = mi;
  tmv.tm_sec = se;
  tmv.tm_isdst = 0;
  return mktime(&tmv);
}

// "4H59M Left" - relative, for the short 5-hour window. Needs the device's
// clock to actually be synced (see configTime() in setup()); until SNTP
// finishes syncing after boot, time(nullptr) is small and this will show a
// large, wrong duration for the first few seconds.
void formatCountdown(char *out, size_t outSize, time_t resetEpoch) {
  if (resetEpoch <= 0) {
    snprintf(out, outSize, "--");
    return;
  }
  long remain = (long)difftime(resetEpoch, time(nullptr));
  // A correctly-synced clock should never see more than ~5h left on this
  // window; a huge value means SNTP hasn't synced yet (time(nullptr) is
  // still near-epoch) and resetEpoch can't be trusted yet either.
  if (remain < 0 || remain > 6L * 3600) {
    snprintf(out, outSize, "--");
    return;
  }
  snprintf(out, outSize, "%ldH%02ldM Left", remain / 3600, (remain % 3600) / 60);
}

// "Fri 24:00" - absolute weekday+time (UTC), for the multi-day weekly
// window, where a countdown in hours would be harder to read at a glance.
void formatResetDay(char *out, size_t outSize, time_t resetEpoch) {
  if (resetEpoch <= 0) {
    snprintf(out, outSize, "--");
    return;
  }
  struct tm tmv;
  gmtime_r(&resetEpoch, &tmv);
  static const char *wdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  snprintf(out, outSize, "%s %02d:%02d", wdays[tmv.tm_wday], tmv.tm_hour, tmv.tm_min);
}

void renderAuthFailedScreen() {
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print("Auth failed!");
  lcd->setCursor(0, 1);
  lcd->print("Redo setup-token");
}

// Forces the next render() to rewrite both rows regardless of whether the
// text looks unchanged - needed after clear()/screen switches/reconnects.
void forceRedraw() {
  renderedRow0[0] = '\0';
  renderedRow1[0] = '\0';
}

void render() {
  if (authFailed) {
    if (!renderedAuthFailed) {
      renderAuthFailedScreen();
      renderedAuthFailed = true;
      forceRedraw();  // so a full redraw happens once auth recovers
    }
    return;
  }

  if (renderedAuthFailed) {
    lcd->clear();
    renderedAuthFailed = false;
  }

  char row0[LCD_COLS + 1];
  char row1[LCD_COLS + 1];
  char detail[LCD_COLS + 1];

  if (currentScreen == 0) {
    if (sessionUtilization < 0) {
      snprintf(row0, sizeof(row0), "5H --%%");
    } else {
      snprintf(row0, sizeof(row0), "5H %d%%%s", constrain(sessionUtilization, 0, 100),
               dataStale ? "!" : "");
    }
    formatCountdown(detail, sizeof(detail), sessionResetEpoch);
  } else {
    if (weeklyUtilization < 0) {
      snprintf(row0, sizeof(row0), "WK --%%");
    } else {
      snprintf(row0, sizeof(row0), "WK %d%%%s", constrain(weeklyUtilization, 0, 100),
               dataStale ? "!" : "");
    }
    formatResetDay(detail, sizeof(detail), weeklyResetEpoch);
  }
  snprintf(row1, sizeof(row1), "%s", detail);

  if (strcmp(row0, renderedRow0) != 0) {
    lcdPrintRow(0, row0);
    strncpy(renderedRow0, row0, LCD_COLS);
    renderedRow0[LCD_COLS] = '\0';
  }
  if (LCD_ROWS > 1 && strcmp(row1, renderedRow1) != 0) {
    lcdPrintRow(1, row1);
    strncpy(renderedRow1, row1, LCD_COLS);
    renderedRow1[LCD_COLS] = '\0';
  }
}

void connectWiFi() {
  lcd->clear();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);

  // Some ESP32-C3 SuperMini/clone boards have a genuine RF hardware
  // defect: bad antenna impedance matching reflects the default 19.5dBm
  // TX power back into the radio and corrupts the auth exchange -
  // reproducing as this exact AUTH_EXPIRE (reason=2) pattern regardless
  // of which AP it's talking to. This project is where that was confirmed
  // (identical failure across 3 unrelated APs and 2 physical boards, which
  // rules out both "this router" and "this board" - see readai.md); the
  // root cause and this workaround are credited to arduino-esp32 #6767.
  // This finding is also what corrected esp32-wifi-fix-kit's own
  // root-cause writeup upstream (an earlier, now-superseded theory there
  // blamed a router-side MAC lockout instead). setTxPower() only takes
  // effect once the STA interface is up, so this has to come after
  // begin() - calling it before is a silent no-op.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  // Live diagnostic view on the LCD itself - SSID + elapsed time on row 0,
  // live status + last disconnect reason on row 1. No Serial connection
  // needed to see exactly what's happening and why.
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(400);
    unsigned long elapsedS = (millis() - start) / 1000;
    Serial.printf("[wifi] status=%s (%lus)\n", wifiStatusName(WiFi.status()), elapsedS);

    char row0[LCD_COLS + 8];
    snprintf(row0, sizeof(row0), "%.11s %2lus", WIFI_SSID, elapsedS);
    lcdPrintRow(0, row0);

    char row1[LCD_COLS + 8];
    snprintf(row1, sizeof(row1), "%s r=%d", wifiStatusName(WiFi.status()), lastDisconnectReason);
    lcdPrintRow(1, row1);
  }

  if (WiFi.status() != WL_CONNECTED) {
    wifiFailCount++;
    wl_status_t finalStatus = WiFi.status();
    Serial.printf(
        "[wifi] FAILED #%d (last status=%s, reason=%d) - checklist: SSID/password correct? "
        "2.4GHz band (ESP32 doesn't support 5GHz)? Router's 2.4GHz channel width set to 20MHz "
        "rather than 40MHz/auto?\n",
        wifiFailCount, wifiStatusName(finalStatus), lastDisconnectReason);

    // Live countdown to the next attempt, still showing the failure detail -
    // this whole screen is the "no Serial needed" failure report.
    unsigned long waitMs = wifiRetryBackoffMs;
    unsigned long waitStart = millis();
    while (millis() - waitStart < waitMs) {
      unsigned long remainS = (waitMs - (millis() - waitStart) + 999) / 1000;

      char row0[LCD_COLS + 8];
      snprintf(row0, sizeof(row0), "FAIL#%d r=%d", wifiFailCount, lastDisconnectReason);
      lcdPrintRow(0, row0);

      char row1[LCD_COLS + 8];
      snprintf(row1, sizeof(row1), "%s %2lus", wifiStatusName(finalStatus), remainS);
      lcdPrintRow(1, row1);

      delay(500);
    }

    wifiRetryBackoffMs = min(wifiRetryBackoffMs * 2, WIFI_RETRY_BACKOFF_MAX_MS);
    return;  // loop()/setup() will call connectWiFi() again since we're still disconnected
  }

  wifiFailCount = 0;
  wifiRetryBackoffMs = WIFI_RETRY_BACKOFF_START_MS;  // reset backoff after a real success
  Serial.printf("[wifi] connected. IP = %s\n", WiFi.localIP().toString().c_str());

  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print("WiFi connected");
  lcd->setCursor(0, 1);
  lcd->print(WiFi.localIP());
  delay(1200);
  lcd->clear();
  forceRedraw();  // screen was just cleared
}

// Sends a minimal (max_tokens=1) request to the real Anthropic Messages API
// using a Claude Code OAuth token, and reads the rate-limit utilization
// straight out of the response headers. No third-party bridge, no scraped
// browser cookie, no Cloudflare bot-detection to fight - this is the same
// official endpoint the Claude Code CLI itself checks.
bool fetchUsage() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setCACert(CA_BUNDLE);

  HTTPClient https;
  if (!https.begin(client, MESSAGES_ENDPOINT)) {
    Serial.println("HTTPS begin() failed");
    return false;
  }

  https.addHeader("Authorization", String("Bearer ") + CLAUDE_OAUTH_TOKEN);
  https.addHeader("anthropic-version", ANTHROPIC_VERSION);
  https.addHeader("anthropic-beta", "oauth-2025-04-20");
  https.addHeader("content-type", "application/json");
  https.addHeader("User-Agent", "claude-code/2.1.5");
  https.setTimeout(HTTP_TIMEOUT_MS);

  static const char *rlHeaders[] = {
      "anthropic-ratelimit-unified-5h-utilization",
      "anthropic-ratelimit-unified-7d-utilization",
      "anthropic-ratelimit-unified-5h-reset",
      "anthropic-ratelimit-unified-7d-reset",
  };
  https.collectHeaders(rlHeaders, 4);

  const char *body =
      "{\"model\":\"" PROBE_MODEL "\",\"max_tokens\":1,"
      "\"messages\":[{\"role\":\"user\",\"content\":\".\"}]}";

  int code = https.POST(body);
  Serial.printf("[API] HTTP %d\n", code);

  if (code <= 0) {
    https.end();
    return false;
  }

  String h5 = https.header("anthropic-ratelimit-unified-5h-utilization");
  String d7 = https.header("anthropic-ratelimit-unified-7d-utilization");
  String r5 = https.header("anthropic-ratelimit-unified-5h-reset");
  String r7 = https.header("anthropic-ratelimit-unified-7d-reset");
  https.end();

  if (h5.length() == 0 && d7.length() == 0) {
    if (code == 401) {
      authFailed = true;
      Serial.println("Auth failed - token invalid/expired. Run `claude setup-token` again.");
    } else {
      Serial.printf("No usage headers in response (HTTP %d)\n", code);
    }
    return false;
  }

  authFailed = false;
  // utilization arrives as 0.0-1.0
  sessionUtilization = (int)round(h5.toFloat() * 100.0f);
  weeklyUtilization = (int)round(d7.toFloat() * 100.0f);
  // Reset headers are assumed ISO8601 UTC (e.g. "2026-07-22T01:00:00Z"),
  // matching the "-reset" suffix convention Anthropic uses elsewhere in its
  // rate-limit headers - not yet confirmed against a live response for the
  // *unified* 5h/7d headers specifically. If a screen shows "--" instead of
  // a real countdown, print r5/r7 to Serial here and check the actual
  // header text/format against parseIso8601Utc() above.
  sessionResetEpoch = parseIso8601Utc(r5.c_str());
  weeklyResetEpoch = parseIso8601Utc(r7.c_str());
  dataStale = false;
  lastContactMs = millis();
  return true;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);

  uint8_t lcdAddress = scanForLcdAddress();
  Serial.printf("LCD I2C address: 0x%02X\n", lcdAddress);

  lcd = new LiquidCrystal_I2C(lcdAddress, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();

  // STA mode, stale-NVS-cache clear, defensive HT20 bandwidth - see
  // github.com/caffentrager/esp32-wifi-fix-kit. Call once here, not on
  // every reconnect: NVS only goes stale across reboots/reflashes, not
  // mid-runtime.
  wifiFix.begin();
  WiFi.onEvent(onWifiEvent);  // additional handler: this project also wants the reason code on the LCD

  connectWiFi();

  // UTC, no DST - needed so time(nullptr) is real wall-clock time, which
  // formatCountdown()/formatResetDay() need to turn the API's reset
  // timestamps into "time left"/"resets when". Runs in the background;
  // takes a few seconds after WiFi comes up, during which countdowns will
  // be briefly wrong until it finishes.
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  lastScreenSwitchMs = millis();
  fetchUsage();
  render();
}

unsigned long lastFetchAttemptMs = 0;

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (lastFetchAttemptMs == 0 || millis() - lastFetchAttemptMs >= REFRESH_INTERVAL_MS) {
    lastFetchAttemptMs = millis();
    fetchUsage();
  }

  if (millis() - lastContactMs > STALE_AFTER_MS) {
    dataStale = true;
  }

  if (millis() - lastScreenSwitchMs >= SCREEN_ROTATE_MS) {
    currentScreen = (currentScreen + 1) % 2;
    lastScreenSwitchMs = millis();
    forceRedraw();  // row content's meaning changes across screens, not just its value
  }

  render();
  delay(1000);
}
