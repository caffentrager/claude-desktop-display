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

// Wiring: one leg to GPIO6, the other to GND. Uses the internal pull-up
// (no external resistor needed). Change this if your board doesn't break
// out GPIO6 - anything free that isn't SDA/SCL or a strapping pin
// (GPIO2/8/9 on ESP32-C3) works.
#define PIN_SCREEN_BUTTON 6
#define BUTTON_DEBOUNCE_MS 250UL

// This calls the real Anthropic API every poll, so don't hammer it. 120s
// matches claude-usage-stick's own default (tested range: 30s-300s).
#define REFRESH_INTERVAL_MS 120000UL
#define HTTP_TIMEOUT_MS 15000UL
#define STALE_AFTER_MS (3UL * REFRESH_INTERVAL_MS)  // flag data as stale after a few missed polls

// The three screens (5-hour detail, weekly detail, combined glance)
// alternate on this interval; the button also advances early on demand.
#define SCREEN_ROTATE_MS 4000UL
#define SCREEN_COUNT 3

// Width, in LCD cells, of the usage bar on row 0 - fixed regardless of
// LCD_COLS, matching the layout designed in lcd_editor.html.
#define BAR_WIDTH 10

// Sub-cell fill steps per bar cell (5 = one HD44780 glyph column each),
// for finer resolution than a plain filled/empty cell would give -
// BAR_WIDTH * BAR_SUBDIV distinct levels across the whole bar.
#define BAR_SUBDIV 5

// Hardcoded for this specific device's desk (Seoul) - only affects the WK
// screen's displayed weekday/time. KST has no DST, so this never needs a
// seasonal adjustment; if this device is ever relocated, change this.
#define DISPLAY_TZ_OFFSET_SEC (9 * 3600)

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

// CGRAM has 8 slots (0-7). Bar uses 0 (full) + 1-4 (partial fill, 1-4 of
// 5 columns lit - see BAR_SUBDIV); 5 is a small decorative sparkle, used
// on the boot splash and tucked into spare columns on the detail screens.
// 6-7 are free.
static const byte FULL_BLOCK_CHAR = 0;
static const byte LOGO_CHAR = 5;
// PARTIAL_FILL_CHARS[i] = the glyph for (i+1) of BAR_SUBDIV columns lit,
// i.e. index 0..3 -> CGRAM slots 1..4 (5/5 lit reuses FULL_BLOCK_CHAR
// instead of a 5th glyph, since that's already an all-columns-lit block).
static const byte PARTIAL_FILL_CHARS[BAR_SUBDIV - 1] = {1, 2, 3, 4};

static byte fullBlockGlyph[8] = {
    0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111,
};
static byte partialFillGlyphs[BAR_SUBDIV - 1][8] = {
    {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000},
    {0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000, 0b11000},
    {0b11100, 0b11100, 0b11100, 0b11100, 0b11100, 0b11100, 0b11100, 0b11100},
    {0b11110, 0b11110, 0b11110, 0b11110, 0b11110, 0b11110, 0b11110, 0b11110},
};
// An original decorative sparkle/starburst, not a reproduction of any
// company's actual logo (5x8 monochrome pixels can't meaningfully
// reproduce one anyway) - just a small "something's sparkly here" touch.
static byte logoGlyph[8] = {
    0b01010, 0b00100, 0b10101, 0b01110, 0b11111, 0b01110, 0b10101, 0b00100,
};

int sessionUtilization = -1;  // 5-hour usage %, -1 = no data yet
int weeklyUtilization = -1;   // 7-day (week) usage %, -1 = no data yet
time_t sessionResetEpoch = 0; // unix time the 5h window resets, 0 = unknown
time_t weeklyResetEpoch = 0;  // unix time the 7d window resets, 0 = unknown
bool dataStale = true;
bool authFailed = false;
unsigned long lastContactMs = 0;

// The display cycles through 3 screens automatically, or on demand via
// PIN_SCREEN_BUTTON (checkScreenButton()) - see readai.md.
int currentScreen = 0;  // 0 = 5H detail, 1 = WK detail, 2 = combined glance
unsigned long lastScreenSwitchMs = 0;

// Tracks what's currently on screen so we only touch the I2C bus when
// something actually changes. Row 0 on the bar screens is a label +
// custom-char bar graph, not plain text, so it's tracked by a small "did
// the inputs change" key instead of the literal row text (a bar's
// on-screen bytes aren't a valid C string - see drawBar()). The combined
// screen's row 0 is plain text, so it gets its own literal-text buffer
// instead of reusing the bar screens' key.
char renderedRow0Key[12] = "";
char renderedCombinedRow0[LCD_COLS + 1] = "";
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

// Draws a fixed BAR_WIDTH-cell bar starting at (col, row) with BAR_SUBDIV
// levels of resolution per cell (partial-fill glyphs for the one cell
// straddling the fill boundary, full/blank for the rest). Always paints
// every cell in that range, so a shrinking percentage can't leave stale
// filled cells behind from a previous, larger reading.
void drawBar(int row, int col, int percent) {
  percent = constrain(percent, 0, 100);
  int filledUnits = (percent * BAR_WIDTH * BAR_SUBDIV + 50) / 100;
  lcd->setCursor(col, row);
  for (int i = 0; i < BAR_WIDTH; i++) {
    int cellUnits = filledUnits - i * BAR_SUBDIV;
    if (cellUnits <= 0) {
      lcd->write(' ');
    } else if (cellUnits >= BAR_SUBDIV) {
      lcd->write(FULL_BLOCK_CHAR);
    } else {
      lcd->write(PARTIAL_FILL_CHARS[cellUnits - 1]);
    }
  }
}

// Parses the reset headers - confirmed on real hardware (2026-07-21) to be
// plain base-10 Unix timestamps (e.g. "1784652600"), not the ISO8601
// string an earlier version of this code assumed. Returns 0 ("unknown",
// rendered as "--") if the string is empty or isn't a positive integer.
time_t parseResetEpoch(const char *s) {
  if (!s || !*s) return 0;
  char *end = nullptr;
  long v = strtol(s, &end, 10);
  if (end == s || v <= 0) return 0;
  return (time_t)v;
}

// "4H59M" (no suffix - callers that want "... Left" append it themselves,
// since the combined glance screen doesn't have room for it) - relative,
// for the short 5-hour window. Needs the device's clock to actually be
// synced (see configTime() in setup()); until SNTP finishes syncing after
// boot, time(nullptr) is small and this will show a large, wrong duration
// for the first few seconds.
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
  snprintf(out, outSize, "%ldH%02ldM", remain / 3600, (remain % 3600) / 60);
}

// "Fri 19:00" - absolute weekday+time in DISPLAY_TZ_OFFSET_SEC (Seoul), for
// the multi-day weekly window, where a countdown in hours would be harder
// to read at a glance. resetEpoch itself (UTC, from the API) is untouched -
// only shifted for this display's benefit, so formatCountdown()'s duration
// math elsewhere isn't affected by this offset.
void formatResetDay(char *out, size_t outSize, time_t resetEpoch) {
  if (resetEpoch <= 0) {
    snprintf(out, outSize, "--");
    return;
  }
  time_t localEpoch = resetEpoch + DISPLAY_TZ_OFFSET_SEC;
  struct tm tmv;
  gmtime_r(&localEpoch, &tmv);
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
  renderedRow0Key[0] = '\0';
  renderedCombinedRow0[0] = '\0';
  renderedRow1[0] = '\0';
}

void formatPercent(char *out, size_t outSize, int percent) {
  if (percent < 0) {
    snprintf(out, outSize, "--");
  } else {
    snprintf(out, outSize, "%d%%", constrain(percent, 0, 100));
  }
}

// Both metrics on one screen, compact: no bars (no room), just numbers -
// see readai.md for why this doesn't fit the "Left" suffix or a stale
// marker the way the dedicated screens do.
void renderCombinedScreen() {
  char p5[5], pWk[5], c5[6], cWk[10];
  formatPercent(p5, sizeof(p5), sessionUtilization);
  formatPercent(pWk, sizeof(pWk), weeklyUtilization);
  formatCountdown(c5, sizeof(c5), sessionResetEpoch);
  formatResetDay(cWk, sizeof(cWk), weeklyResetEpoch);

  // No space between label and percent (unlike the dedicated screens) -
  // needed to stay within LCD_COLS at 100% ("WK100% Fri 19:00" is exactly
  // 16 chars; "WK 100% Fri 19:00" would be 17 and get silently truncated).
  char row0[LCD_COLS + 1];
  char row1[LCD_COLS + 1];
  snprintf(row0, sizeof(row0), "5H%s %s", p5, c5);
  snprintf(row1, sizeof(row1), "WK%s %s", pWk, cWk);

  if (strcmp(row0, renderedCombinedRow0) != 0) {
    lcdPrintRow(0, row0);
    strncpy(renderedCombinedRow0, row0, LCD_COLS);
    renderedCombinedRow0[LCD_COLS] = '\0';
  }
  if (LCD_ROWS > 1 && strcmp(row1, renderedRow1) != 0) {
    lcdPrintRow(1, row1);
    strncpy(renderedRow1, row1, LCD_COLS);
    renderedRow1[LCD_COLS] = '\0';
  }
}

// Row 0: "5H"/"WK" label + a BAR_WIDTH-cell bar graph of the percentage,
// plus a decorative sparkle in the spare columns to the right of the bar.
// Row 1: the percentage as a number + the countdown/reset-day detail.
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

  if (currentScreen == 2) {
    renderCombinedScreen();
    return;
  }

  const char *label = currentScreen == 0 ? "5H" : "WK";
  int percent = currentScreen == 0 ? sessionUtilization : weeklyUtilization;

  char row0Key[12];
  snprintf(row0Key, sizeof(row0Key), "%s%d%d", label, percent, dataStale ? 1 : 0);
  if (strcmp(row0Key, renderedRow0Key) != 0) {
    lcd->setCursor(0, 0);
    lcd->print(label);
    lcd->print(' ');
    drawBar(0, strlen(label) + 1, percent < 0 ? 0 : percent);
    lcd->print(' ');
    lcd->write(LOGO_CHAR);
    strncpy(renderedRow0Key, row0Key, sizeof(renderedRow0Key) - 1);
    renderedRow0Key[sizeof(renderedRow0Key) - 1] = '\0';
  }

  char pctStr[5];
  formatPercent(pctStr, sizeof(pctStr), percent);

  char detail[LCD_COLS + 1];
  if (currentScreen == 0) {
    char countdown[8];
    formatCountdown(countdown, sizeof(countdown), sessionResetEpoch);
    if (strcmp(countdown, "--") == 0) {
      snprintf(detail, sizeof(detail), "--");
    } else {
      snprintf(detail, sizeof(detail), "%s Left", countdown);
    }
  } else {
    formatResetDay(detail, sizeof(detail), weeklyResetEpoch);
  }

  char row1[LCD_COLS + 1];
  snprintf(row1, sizeof(row1), "%s %s%s", pctStr, detail, dataStale && percent >= 0 ? "!" : "");

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
  Serial.printf("[API] 5h=%s reset5h='%s' 7d=%s reset7d='%s'\n", h5.c_str(), r5.c_str(),
                d7.c_str(), r7.c_str());

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
  sessionResetEpoch = parseResetEpoch(r5.c_str());
  weeklyResetEpoch = parseResetEpoch(r7.c_str());
  dataStale = false;
  lastContactMs = millis();
  return true;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL);
  pinMode(PIN_SCREEN_BUTTON, INPUT_PULLUP);

  uint8_t lcdAddress = scanForLcdAddress();
  Serial.printf("LCD I2C address: 0x%02X\n", lcdAddress);

  lcd = new LiquidCrystal_I2C(lcdAddress, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
  lcd->createChar(FULL_BLOCK_CHAR, fullBlockGlyph);
  for (int i = 0; i < BAR_SUBDIV - 1; i++) {
    lcd->createChar(PARTIAL_FILL_CHARS[i], partialFillGlyphs[i]);
  }
  lcd->createChar(LOGO_CHAR, logoGlyph);

  lcd->setCursor(0, 0);
  lcd->print("ClaudeMeter ");
  lcd->write(LOGO_CHAR);
  delay(800);

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

// Standard Arduino debounce pattern: the raw pin reading has to sit still
// for BUTTON_DEBOUNCE_MS before a change counts, so switch bounce doesn't
// register as multiple presses. PIN_SCREEN_BUTTON is INPUT_PULLUP, so a
// press reads LOW.
int buttonDebouncedState = HIGH;
int buttonLastReading = HIGH;
unsigned long buttonLastChangeMs = 0;

void checkScreenButton() {
  int reading = digitalRead(PIN_SCREEN_BUTTON);
  if (reading != buttonLastReading) {
    buttonLastChangeMs = millis();
  }
  if (millis() - buttonLastChangeMs > BUTTON_DEBOUNCE_MS && reading != buttonDebouncedState) {
    buttonDebouncedState = reading;
    if (buttonDebouncedState == LOW) {
      currentScreen = (currentScreen + 1) % SCREEN_COUNT;
      lastScreenSwitchMs = millis();  // don't also auto-advance right after a manual press
      forceRedraw();
    }
  }
  buttonLastReading = reading;
}

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

  checkScreenButton();

  if (millis() - lastScreenSwitchMs >= SCREEN_ROTATE_MS) {
    currentScreen = (currentScreen + 1) % SCREEN_COUNT;
    lastScreenSwitchMs = millis();
    forceRedraw();  // row content's meaning changes across screens, not just its value
  }

  render();
  delay(1000);
}
