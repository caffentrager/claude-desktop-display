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
// This device's LCD was assumed to be 18x2 at the start of the project,
// but only 16 columns are actually visible on the real glass (confirmed
// 2026-07-22 via the serial debug console's "r" command: a distinct
// character printed in every column, columns 16-17 never showed up).
// 20x4 also works - just change these two; BAR_WIDTH below scales to fill
// whatever width is set, so the bar always uses the full row instead of
// leaving it partly blank.
#define LCD_COLS 16
#define LCD_ROWS 2

// Wiring: SDA -> GPIO4, SCL -> GPIO5
#define PIN_SDA 4
#define PIN_SCL 5

// Wiring: one leg to GPIO20, the other to GND. Uses the internal pull-up
// (no external resistor needed). GPIO6 and GPIO7 were both tried first and
// didn't respond reliably - see checkScreenButton()'s comment for why that
// was actually a polling-rate bug, not a pin problem, so this switch alone
// probably wasn't the fix. GPIO20 is normally UART0 RX, but this board's
// Serial goes over native USB-CDC instead (ARDUINO_USB_MODE=1 in
// platformio.ini), so UART0 is unused and GPIO20 is free.
#define PIN_SCREEN_BUTTON 20
#define BUTTON_DEBOUNCE_MS 250UL

// Second button, same wiring style (one leg to GND, internal pull-up) -
// GPIO21 is UART0 TX, unused for the same reason GPIO20 (UART0 RX) is
// free. Enters an interactive serial-driven debug console
// (runSerialDebugMode()) instead of switching screens: set or step
// through an arbitrary percent/countdown/weekday/time value from the
// serial monitor and it renders immediately using the same primitives the
// real 5H/WK screens use, so any value can be inspected on demand instead
// of waiting for real usage data or the clock to pass through it.
#define PIN_DEBUG_BUTTON 21

// This calls the real Anthropic API every poll, so don't hammer it. 120s
// matches -usage-stick's own default (tested range: 30s-300s).
#define REFRESH_INTERVAL_MS 120000UL
#define HTTP_TIMEOUT_MS 15000UL
#define STALE_AFTER_MS (3UL * REFRESH_INTERVAL_MS)  // flag data as stale after a few missed polls

// Screens alternate on this interval; the button also advances early on
// demand. SCREEN_COUNT is 2 (5H, WK) - the combined-glance screen
// (renderCombinedScreen(), currentScreen==2) is implemented but on hold
// per explicit request; bump this back to 3 to bring it back, nothing
// else needs to change.
#define SCREEN_ROTATE_MS 4000UL
#define SCREEN_COUNT 2

// Width, in LCD cells, of the usage bar on row 0 - scales to use every
// column: row 0 is "<label> <bar> <logo>", i.e. 2 (label) + 1 (gap) +
// BAR_WIDTH + 1 (gap) + 1 (logo) = LCD_COLS exactly, whatever LCD_COLS is.
#define BAR_WIDTH (LCD_COLS - 5)

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

// CGRAM has 8 slots (0-7), 1 free (7): 0 (full) + 1-4 (partial fill, 1-4
// of 5 columns lit - see BAR_SUBDIV) + 5 (decorative sparkle, boot splash
// + detail-screen row 0/1) + 6 (empty-cell top/bottom guide line, used
// uniformly for every empty cell - no special first/last-cell treatment;
// an earlier version gave the bar's last cell its own bracket-shaped
// end-cap glyph, but that needed a 9th slot to also add a matching
// start-cap for the first cell, which doesn't exist - simplified back to
// one glyph for all empty cells on request instead of sacrificing
// something else (the logo, bar resolution, or this border consistency
// itself) to make room).
// FULL_BLOCK_CHAR briefly moved to the HD44780 ROM's built-in solid
// block (0xFF, no CGRAM slot needed on ROM code A00) - moved back to
// CGRAM so it could get the same rows-1/6 inset treatment as the other
// glyphs below; a ROM glyph's bit pattern can't be customized, so the
// two goals (free a slot vs. consistent look) were mutually exclusive,
// and consistent look won out on request.
static const byte FULL_BLOCK_CHAR = 0;
static const byte LOGO_CHAR = 5;
static const byte EMPTY_MID_CHAR = 6;
// PARTIAL_FILL_CHARS[i] = the glyph for (i+1) of BAR_SUBDIV columns lit,
// i.e. index 0..3 -> CGRAM slots 1..4 (5/5 lit reuses FULL_BLOCK_CHAR
// instead of a 5th glyph, since that's already an all-columns-lit block).
static const byte PARTIAL_FILL_CHARS[BAR_SUBDIV - 1] = {1, 2, 3, 4};

// Same border+inset treatment as the partial-fill glyphs below: rows 0/7
// are the continuous top/bottom border, rows 1/6 are the blank inset gap,
// and rows 2-5 (the "content") are solid since this represents 100% full.
static byte fullBlockGlyph[8] = {
    0b11111, 0b00000, 0b11111, 0b11111, 0b11111, 0b11111, 0b00000, 0b11111,
};

// Row 0 and row 7 are the border (full-width, matching FULL_BLOCK_CHAR
// and the empty-cell glyphs below, so the bar's top/bottom line runs
// continuously across every cell regardless of fill state); rows 1 and 6
// are always left blank as a one-pixel inset gap, with the actual fill
// indicator confined to rows 2-5 - a "recessed/framed" look, closer to a
// loading-bar style than a flat block flush against its own border.
static byte partialFillGlyphs[BAR_SUBDIV - 1][8] = {
    {0b11111, 0b00000, 0b10000, 0b10000, 0b10000, 0b10000, 0b00000, 0b11111},
    {0b11111, 0b00000, 0b11000, 0b11000, 0b11000, 0b11000, 0b00000, 0b11111},
    {0b11111, 0b00000, 0b11100, 0b11100, 0b11100, 0b11100, 0b00000, 0b11111},
    {0b11111, 0b00000, 0b11110, 0b11110, 0b11110, 0b11110, 0b00000, 0b11111},
};
// An original decorative sparkle/starburst, not a reproduction of any
// company's actual logo (5x8 monochrome pixels can't meaningfully
// reproduce one anyway) - just a small "something's sparkly here" touch.
static byte logoGlyph[8] = {
    0b01010, 0b00100, 0b10101, 0b01110, 0b11111, 0b01110, 0b10101, 0b00100,
};
// Just a top and bottom line, no sides - marks the bar's height without
// boxing in every empty cell. Used for every empty cell uniformly,
// including the bar's first and last cells - see the CGRAM comment above
// for why there's no separate bracket-shaped cap for those positions.
static byte emptyMidGlyph[8] = {
    0b11111, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b11111,
};

int sessionUtilization = -1;  // 5-hour usage %, -1 = no data yet
int weeklyUtilization = -1;   // 7-day (week) usage %, -1 = no data yet
time_t sessionResetEpoch = 0; // unix time the 5h window resets, 0 = unknown
time_t weeklyResetEpoch = 0;  // unix time the 7d window resets, 0 = unknown
bool dataStale = true;
bool authFailed = false;
unsigned long lastContactMs = 0;

// The display cycles through SCREEN_COUNT screens automatically, or on
// demand via PIN_SCREEN_BUTTON (checkScreenButton()) - see readai.md.
int currentScreen = 0;  // 0 = 5H detail, 1 = WK detail, 2 = combined glance (currently unused, SCREEN_COUNT=2)
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
// straddling the fill boundary, full/empty for the rest). Always paints
// every cell in that range, so a shrinking percentage can't leave stale
// filled cells behind from a previous, larger reading. Every empty cell
// (first, middle, or last) uses the same EMPTY_MID_CHAR (just a
// top/bottom line) - no special bracket-shaped cap on the bar's edges;
// see the CGRAM comment near EMPTY_MID_CHAR for why.
void drawBar(int row, int col, int percent) {
  percent = constrain(percent, 0, 100);
  int filledUnits = (percent * BAR_WIDTH * BAR_SUBDIV + 50) / 100;
  lcd->setCursor(col, row);
  for (int i = 0; i < BAR_WIDTH; i++) {
    int cellUnits = filledUnits - i * BAR_SUBDIV;
    if (cellUnits <= 0) {
      lcd->write(EMPTY_MID_CHAR);
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
  // Lowercase with a space ("4h 5m") rather than "4H5M" - reads more like
  // ordinary text at a glance. Minutes still aren't zero-padded ("4h 5m",
  // not "4h 05m"). Once under an hour left, the hour part is dropped
  // entirely ("5m", not "0h 5m") rather than showing a zero hour count.
  long hours = remain / 3600;
  long mins = (remain % 3600) / 60;
  if (hours == 0) {
    snprintf(out, outSize, "%ldm", mins);
  } else {
    snprintf(out, outSize, "%ldh %ldm", hours, mins);
  }
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
// plus a sparkle in the last column that doubles as the freshness
// indicator - LOGO_CHAR when the data's current, blank when dataStale.
// Row 1: the percentage as a number + the countdown/reset-day detail,
// plain text, no decoration - this device's LCD only has 16 real columns
// (see LCD_COLS above), which doesn't leave reliable room for a trailing
// sparkle here across every percent/countdown-length combination, so the
// staleness signal moved off row 1's variable-length end: row 0's logo
// blanks out, and row 1 shows "OLD" in place of the countdown/reset-day
// detail (see below) - two fixed-width signals instead of an appended "!"
// that could push row 1 past the visible edge.
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
    if (dataStale) {
      lcd->print(' ');
    } else {
      lcd->write(LOGO_CHAR);
    }
    strncpy(renderedRow0Key, row0Key, sizeof(renderedRow0Key) - 1);
    renderedRow0Key[sizeof(renderedRow0Key) - 1] = '\0';
  }

  char pctStr[5];
  formatPercent(pctStr, sizeof(pctStr), percent);

  // When stale, "OLD" replaces the countdown/reset-day detail outright
  // (rather than being appended to it) - short enough to never risk
  // pushing row 1 past the visible edge regardless of percent width, and
  // paired with the blank row-0 logo above for a second, harder-to-miss
  // signal than either alone.
  char detail[LCD_COLS + 1];
  if (dataStale && percent >= 0) {
    snprintf(detail, sizeof(detail), "OLD");
  } else if (currentScreen == 0) {
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
  snprintf(row1, sizeof(row1), "%s %s", pctStr, detail);

  if (LCD_ROWS > 1 && strcmp(row1, renderedRow1) != 0) {
    lcdPrintRow(1, row1);
    strncpy(renderedRow1, row1, LCD_COLS);
    renderedRow1[LCD_COLS] = '\0';
  }
}

// One "field" the serial debug console can set or step through; +/- acts
// on whichever field a preceding p/t/w/k command last touched.
enum DebugField { DEBUG_FIELD_PERCENT, DEBUG_FIELD_COUNTDOWN, DEBUG_FIELD_WEEKDAY, DEBUG_FIELD_TIME };

// Renders one frame of the serial debug console using the exact same
// primitives (drawBar(), the real screens use for row 0), then echoes
// row1 back over Serial. Row 0 is only echoed as its label/percent, not
// reconstructed as text, since its bar cells are CGRAM bytes with no
// printable representation and the bar's own fill arithmetic (drawBar())
// isn't what's in question here.
void renderDebugScreen(int screen, int percent, long countdownSec, int weekdayMonFirst, int timeHour,
                        int timeMin, bool stale) {
  static const char *wdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const int mondayFirst[] = {1, 2, 3, 4, 5, 6, 0};  // command index (0=Mon) -> wdays[] index

  const char *label = screen == 0 ? "5H" : "WK";
  lcd->setCursor(0, 0);
  lcd->print(label);
  lcd->print(' ');
  drawBar(0, strlen(label) + 1, percent);
  lcd->print(' ');
  if (stale) {
    lcd->print(' ');
  } else {
    lcd->write(LOGO_CHAR);
  }

  char pctStr[5];
  formatPercent(pctStr, sizeof(pctStr), percent);

  char detail[LCD_COLS + 1];
  if (stale) {
    snprintf(detail, sizeof(detail), "OLD");
  } else if (screen == 0) {
    long hours = countdownSec / 3600;
    long mins = (countdownSec % 3600) / 60;
    char countdown[8];
    if (hours == 0) {
      snprintf(countdown, sizeof(countdown), "%ldm", mins);
    } else {
      snprintf(countdown, sizeof(countdown), "%ldh %ldm", hours, mins);
    }
    snprintf(detail, sizeof(detail), "%s Left", countdown);
  } else {
    snprintf(detail, sizeof(detail), "%s %02d:%02d", wdays[mondayFirst[weekdayMonFirst]], timeHour, timeMin);
  }

  char row1[LCD_COLS + 1];
  snprintf(row1, sizeof(row1), "%s %s", pctStr, detail);
  lcdPrintRow(1, row1);

  Serial.print(F("[DBG] "));
  Serial.print(label);
  Serial.print(F(" percent="));
  Serial.print(percent);
  Serial.print(stale ? F(" stale=1") : F(" stale=0"));
  Serial.print(F(" row1=\""));
  Serial.print(row1);
  Serial.print(F("\" len="));
  Serial.println(strlen(row1));
}

// Interactive serial-driven debug console, triggered by PIN_DEBUG_BUTTON.
// Set or step through an arbitrary percent/countdown/weekday/time value
// from the serial monitor and it renders immediately via
// renderDebugScreen() (the same primitives the real screens use), so any
// value - including ones nobody thought to hardcode - can be inspected on
// demand instead of waiting for live usage data or the real clock to pass
// through it. Blocking (like connectWiFi()'s diagnostic screens); "q"
// exits back to normal operation via forceRedraw().
//
// Commands, one per line:
//   p<0-100>   set percent (applies to whichever screen is showing)
//   t<h>:<m>   5H screen, set countdown to h hours m minutes left
//   w<0-6>     WK screen, set weekday (0=Mon..6=Sun)
//   k<h>:<m>   WK screen, set reset time of day
//   +  -       step the last-set field (percent/countdown/weekday/time) by one unit
//   r<text>    print <text> on row 1 as-is, bypassing all layout logic -
//              for mapping which columns are actually visible on the glass
//   s          toggle the stale indicator (blank row-0 logo + row-1 "OLD")
//   q          quit back to normal operation
void runSerialDebugMode() {
  int screen = 0;
  DebugField field = DEBUG_FIELD_PERCENT;
  int percent = 50;
  long countdownSec = 65L * 60;  // 1h 5m - an arbitrary non-edge starting point
  int weekday = 0;               // 0=Mon..6=Sun
  int timeHour = 9, timeMin = 0;
  bool stale = false;

  lcd->clear();
  Serial.println(
      F("[DBG] serial debug mode - commands: p<0-100> t<h>:<m> w<0-6> k<h>:<m> r<text> s + - q"));
  renderDebugScreen(screen, percent, countdownSec, weekday, timeHour, timeMin, stale);

  while (true) {
    if (!Serial.available()) {
      delay(20);
      continue;
    }

    char line[24];
    size_t n = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
    char *p = line;
    while (*p == ' ') p++;
    if (*p == '\0') continue;

    if (*p == 'q' || *p == 'Q') {
      break;
    } else if (*p == '+' || *p == '-') {
      int dir = (*p == '+') ? 1 : -1;
      switch (field) {
        case DEBUG_FIELD_PERCENT:
          percent = constrain(percent + dir, 0, 100);
          break;
        case DEBUG_FIELD_COUNTDOWN:
          countdownSec = max(0L, countdownSec + dir * 60L);
          break;
        case DEBUG_FIELD_WEEKDAY:
          weekday = ((weekday + dir) % 7 + 7) % 7;
          break;
        case DEBUG_FIELD_TIME:
          timeMin += dir;
          if (timeMin >= 60) {
            timeMin = 0;
            timeHour = (timeHour + 1) % 24;
          } else if (timeMin < 0) {
            timeMin = 59;
            timeHour = (timeHour + 23) % 24;
          }
          break;
      }
    } else if (*p == 'p' || *p == 'P') {
      percent = constrain(atoi(p + 1), 0, 100);
      field = DEBUG_FIELD_PERCENT;
    } else if (*p == 't' || *p == 'T') {
      long h = 0, m = 0;
      sscanf(p + 1, "%ld:%ld", &h, &m);
      countdownSec = max(0L, h * 3600 + m * 60);
      screen = 0;
      field = DEBUG_FIELD_COUNTDOWN;
    } else if (*p == 'w' || *p == 'W') {
      weekday = ((atoi(p + 1) % 7) + 7) % 7;
      screen = 1;
      field = DEBUG_FIELD_WEEKDAY;
    } else if (*p == 'k' || *p == 'K') {
      int h = 0, m = 0;
      sscanf(p + 1, "%d:%d", &h, &m);
      timeHour = ((h % 24) + 24) % 24;
      timeMin = ((m % 60) + 60) % 60;
      screen = 1;
      field = DEBUG_FIELD_TIME;
    } else if (*p == 'r' || *p == 'R') {
      // Raw diagnostic: prints exactly what follows "r" on row 1, one
      // char per column, bypassing all layout logic - for mapping which
      // columns are actually visible on the physical glass (e.g. send
      // "r0123456789ABCDEFGH" and ask which trailing letters disappear).
      lcdPrintRow(1, p + 1);
      Serial.print(F("[DBG] raw row1=\""));
      Serial.print(p + 1);
      Serial.print(F("\" len="));
      Serial.println(strlen(p + 1));
      continue;
    } else if (*p == 's' || *p == 'S') {
      stale = !stale;
    } else {
      Serial.println(F("[DBG] unknown command"));
      continue;
    }
    renderDebugScreen(screen, percent, countdownSec, weekday, timeHour, timeMin, stale);
  }

  lcd->clear();
  forceRedraw();
  Serial.println(F("[DBG] exiting debug mode"));
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
  pinMode(PIN_DEBUG_BUTTON, INPUT_PULLUP);

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
  lcd->createChar(EMPTY_MID_CHAR, emptyMidGlyph);

  lcd->setCursor(0, 0);
  // write() (raw byte -> CGRAM code) for the logo, print() (formats its
  // argument) for the string - not interchangeable here: print(LOGO_CHAR)
  // would print the literal digit "5" instead of the sparkle, since
  // LiquidCrystal_I2C's print(unsigned char) formats it as a number; and
  // this library's write() only accepts a single uint8_t, not a string,
  // so print() is the only option for "Claude ".
  lcd->write(LOGO_CHAR);
  lcd->print(" Claude ");
  if (LCD_ROWS > 1) {
    lcd->setCursor(0, 1);
    lcd->print("Desktop Display");
  }
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
// press reads LOW. This only works if loop() calls this often enough to
// actually catch a tap - see the delay(20) note at the bottom of loop().
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

int debugButtonDebouncedState = HIGH;
int debugButtonLastReading = HIGH;
unsigned long debugButtonLastChangeMs = 0;

void checkDebugButton() {
  int reading = digitalRead(PIN_DEBUG_BUTTON);
  if (reading != debugButtonLastReading) {
    debugButtonLastChangeMs = millis();
  }
  if (millis() - debugButtonLastChangeMs > BUTTON_DEBOUNCE_MS && reading != debugButtonDebouncedState) {
    debugButtonDebouncedState = reading;
    if (debugButtonDebouncedState == LOW) {
      runSerialDebugMode();
    }
  }
  debugButtonLastReading = reading;
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
  checkDebugButton();

  if (millis() - lastScreenSwitchMs >= SCREEN_ROTATE_MS) {
    currentScreen = (currentScreen + 1) % SCREEN_COUNT;
    lastScreenSwitchMs = millis();
    forceRedraw();  // row content's meaning changes across screens, not just its value
  }

  render();
  // Was delay(1000): everything above is already millis()-gated to its own
  // interval (fetch/stale/rotate all check real elapsed time, not "how many
  // loop iterations"), so that didn't rate-limit them - it just meant
  // checkScreenButton() only sampled the pin once a second. A tap shorter
  // than that window (i.e., basically any normal tap) landed entirely
  // between two samples and was never seen at all - this, not the GPIO
  // choice, is almost certainly why the button "didn't work well" before.
  // 20ms keeps the button responsive; render()'s own change-detection
  // means calling it this much more often doesn't add real I2C traffic.
  delay(20);
}
