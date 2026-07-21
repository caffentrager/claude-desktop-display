# readai.md — context for an AI assistant picking this up cold

## What this is

**ClaudeMeter LCD**: an ESP32-C3 + I2C character-LCD desk gadget that shows
Claude Code's 5-hour and weekly rate-limit usage, polling
`api.anthropic.com` directly with a Claude Code OAuth token (`claude
setup-token`) - no PC has to stay running. See `README.md` for user-facing
setup/wiring/troubleshooting.

- Repo name is `claude-desktop-display`; the project's own README/LCD
  banner call it "ClaudeMeter LCD" (see "Naming note" below) - same
  project, two labels.
- Everything firmware-related lives in `firmware/` (PlatformIO project,
  `env:esp32-c3`, board id `esp32-c3-devkitm-1` but tested on SuperMini
  clones too).
- `lcd_editor.html` (repo root) is a standalone, offline HTML/JS layout
  mockup tool - type into an 18-column x 2-row grid, get back matching
  `lcd.setCursor()`/`lcd.print()` C++ snippets. No build, no server, no
  dependency on the rest of this repo; it's a design aid, not something
  the firmware includes or runs. The screen layouts in `main.cpp`'s
  `render()` were designed with it.

## Key design decisions

- **Direct-API over a bridge script**: reuses the Claude Code CLI's own
  OAuth token and rate-limit-header technique (from
  [oauramos/claude-usage-stick](https://github.com/oauramos/claude-usage-stick))
  to talk straight to `api.anthropic.com`. No second machine to keep
  awake, no scraping claude.ai's cookie-gated internal web API (which is
  what the original macOS-only
  [eddmann/ClaudeMeter](https://github.com/eddmann/ClaudeMeter) inspiration
  does).
- Usage numbers come from the **response headers**
  (`anthropic-ratelimit-unified-5h-utilization` /
  `-7d-utilization`) of a minimal `max_tokens:1` POST, not a JSON body -
  `ArduinoJson` was a dependency early on and was dropped once this was
  confirmed (see the comment in `firmware/platformio.ini`).
- LCD is HD44780 over a PCF8574 I2C backpack; firmware auto-scans the I2C
  bus for the backpack's address (`scanForLcdAddress()` in `main.cpp`)
  rather than hardcoding `0x27` vs `0x3F`. This device's actual LCD is
  18x2 (`LCD_COLS`/`LCD_ROWS` default) - 16x2/20x4 also work by changing
  those two constants; `BAR_WIDTH` derives from `LCD_COLS` so the bar
  always fills the row exactly, see below.
- `render()` only touches the I2C bus when something actually changed.
  Row 0 (label + bar) is tracked by a small change-detection key
  (`renderedRow0Key`) rather than literal text, since the bar's on-screen
  bytes (a mix of a custom CGRAM character and spaces) aren't representable
  as a normal C string; row 1 is plain text, tracked in `renderedRow1` the
  straightforward way.

## Display: 3 screens (5H, WK, combined glance), button-switchable

The LCD cycles through 3 screens (`currentScreen`, 0/1/2), advancing every
`SCREEN_ROTATE_MS` from `loop()` **or** on a press of `PIN_SCREEN_BUTTON`
(debounced in `checkScreenButton()`, standard Arduino debounce pattern -
raw reading must sit still for `BUTTON_DEBOUNCE_MS` before it counts).
A button press also resets `lastScreenSwitchMs`, so the automatic timer
doesn't immediately advance again right on top of a manual one. Note the
button is only polled once per `loop()` iteration, which ends in
`delay(1000)` - so there's up to ~1s + `BUTTON_DEBOUNCE_MS` latency between
a press and the screen changing; this is a deliberate non-goal (nothing
about this device is latency-sensitive) not an overlooked bug.

Screens 0/1 ("5H"/"WK" detail, designed with `lcd_editor.html` - see
below) are each 2 rows:

- Row 0: label + a `BAR_WIDTH`-cell bar graph of the percentage, drawn
  with `drawBar()`, plus a decorative sparkle (`LOGO_CHAR`). `BAR_WIDTH`
  is `LCD_COLS - 5` (not a fixed number) so `"<label> <bar> <sparkle>"`
  (2 + 1 + BAR_WIDTH + 1 + 1) always totals exactly `LCD_COLS` - the bar
  fills whatever's left over instead of leaving unused columns, whatever
  LCD_COLS is set to.
- Row 1: the exact percentage as a number, plus a time detail:
  - Screen 0 ("5H"): `formatCountdown()` - a relative "3H12M" countdown to
    when the 5-hour window resets (screen 0's `render()` branch appends
    " Left" itself; `formatCountdown()`'s raw output has no suffix so
    screen 2 can reuse it without one - see below). Relative reads better
    for a short window.
  - Screen 1 ("WK"): `formatResetDay()` - an absolute "Fri 19:00" (weekday
    + time, shifted by `DISPLAY_TZ_OFFSET_SEC`) for when the 7-day window
    resets. Absolute reads better than a multi-day countdown.
    `DISPLAY_TZ_OFFSET_SEC` is hardcoded to `9*3600` (KST, this device's
    desk) per explicit user request rather than made configurable/derived
    from anything - only shifts the *displayed* value passed to
    `gmtime_r()`, not `resetEpoch` itself, so it can't skew
    `formatCountdown()`'s duration math (which stays pure UTC-vs-UTC).

Screen 2 (`renderCombinedScreen()`) shows both metrics on one screen, no
rotation needed for a quick glance: `"5H<pct> <countdown>"` /
`"WK<pct> <resetday>"`. Two things worth knowing if you touch this:
- **No space between the label and percent** (`"5H100%"`, not
  `"5H 100%"`) - deliberate, not a typo. With the space, `"WK100% Fri
  19:00"` is 16 chars but `"WK 100% Fri 19:00"` is 17, which silently
  truncates on a 16-col display exactly when weeklyUtilization is 100 -
  a real, reachable value, not just a theoretical edge case. Verified via
  a host-side Python length check before flashing (see git history for
  the fix commit). If you add anything to this screen, recheck the
  100%-on-both-metrics case fits.
- No bar graphs (no room) and no stale/`!` marker - this screen trades
  those for compactness; check the dedicated screens for that detail.

CGRAM (8 slots, 0-7) is now down to 1 free slot:
- 0: full-block (`FULL_BLOCK_CHAR`) - bar cells that are completely filled.
- 1-4: partial-fill glyphs (`PARTIAL_FILL_CHARS`, 1-4 of `BAR_SUBDIV`=5
  columns lit) - only the one bar cell straddling the fill boundary ever
  uses one of these; `drawBar()` computes `filledUnits` in units of
  1/`BAR_SUBDIV` of a cell rather than whole cells, for
  `BAR_WIDTH * BAR_SUBDIV` distinct bar levels instead of just `BAR_WIDTH`.
- 5: `LOGO_CHAR`, a small original sparkle/starburst (not a reproduction
  of any real logo - 5x8 monochrome pixels can't meaningfully do that
  anyway), shown on the boot splash (`"Claude <sparkle>"` / `"Desktop
  Display"`, two rows, in `setup()` before WiFi connects) and at the end
  of row 0 on screens 0/1.
- 6: `EMPTY_CELL_CHAR`, a hollow-rectangle outline - `drawBar()` writes
  this instead of a plain space for any cell with 0 units filled, so the
  bar's full width (its "empty slot" boundary) stays visible even at 0%,
  not just once something's filled.
- 7: the last free slot.

This depends on two things that didn't exist before this design:

1. **NTP time sync** (`configTime(0, 0, ...)` in `setup()`) - only
   `formatCountdown()` (the 5H screen) needs this, since it needs the
   device's own current time to compute a remaining duration.
   `formatResetDay()` does NOT need it - `gmtime_r()` just decodes the
   API-provided timestamp itself, independent of "now". Before SNTP
   finishes (a few seconds after WiFi comes up), `time(nullptr)` is
   near-epoch; `formatCountdown()` guards against this by refusing to show
   a countdown longer than 6 hours (a correctly-synced 5h window should
   never read more than that), falling back to `--` instead of a
   multi-year garbage duration.
2. **Two extra response headers**: `anthropic-ratelimit-unified-5h-reset`
   and `-7d-reset`. **Confirmed on real hardware on 2026-07-21** (flashed
   to the board at `/dev/ttyACM0`, MAC `1C:DB:D4:3B:E2:88`, and read back
   over serial) to be **plain base-10 Unix timestamps** (e.g.
   `"1784652600"`), parsed by `parseResetEpoch()` (`strtol`, no string
   parsing needed). An earlier version of this code guessed these were
   ISO8601 strings like `"2026-07-22T01:00:00Z"` and shipped a
   `sscanf`/`mktime()`-based parser for that format - wrong guess, replaced
   once real data was captured. If a future Anthropic API change alters
   this format again, `fetchUsage()` logs the raw header text every poll
   (`[API] 5h=... reset5h='...' 7d=... reset7d='...'`), so check that
   first rather than re-guessing.

## WiFi reliability: this project corrected esp32-wifi-fix-kit upstream

This is the part most likely to surprise a future reader:

- `firmware/lib/Esp32WifiFix/` is a **vendored copy** of the library from
  [caffentrager/esp32-wifi-fix-kit](https://github.com/caffentrager/esp32-wifi-fix-kit),
  used in `setup()` (`wifiFix.begin()`) for stale-NVS-cache clearing and
  defensive HT20 bandwidth forcing.
- The actual WiFi *connect* loop in `main.cpp`'s `connectWiFi()` is
  hand-rolled, **not** a call to the library's `connect()`/
  `beginResilient()` - this project wants a live per-second countdown and
  disconnect-reason readout on the LCD itself (no Serial connection needed
  to see what's happening), which the library's API doesn't expose a hook
  for. If the library gains new fixes upstream in the future, they land in
  `Esp32WifiFix.cpp`'s `connect()`/`beginResilient()`/`loopResilient()` -
  this file calls neither, so re-check `connectWiFi()` by hand instead of
  assuming a re-vendor alone picks up new fixes.
- **The AUTH_EXPIRE (reason=2) antenna-defect root cause now documented in
  esp32-wifi-fix-kit's own `readai.md` (2026-07-21 entry) was found on
  *this* project**: identical WiFi auth failures reproduced across 3
  unrelated APs and 2 physical boards, which ruled out router-side causes
  - an earlier, now-superseded hypothesis in that kit blamed a router-side
  MAC lockout instead. Real cause: some ESP32-C3 SuperMini/clone boards
  have an antenna impedance-matching defect that reflects the default
  19.5dBm TX power back into the radio and corrupts the auth handshake -
  see [arduino-esp32 #6767](https://github.com/espressif/arduino-esp32/issues/6767).
  Fix already applied in `connectWiFi()`:
  `WiFi.setTxPower(WIFI_POWER_8_5dBm)` called immediately *after*
  `WiFi.begin()` (calling it before is a silent no-op - the STA interface
  isn't up yet). Confirmed working on real hardware: first connection
  attempt succeeded on a board+network combination that had failed 100% of
  the time until then.
- If `firmware/lib/Esp32WifiFix/` is ever re-synced from upstream, diff it
  first - this project's `connectWiFi()` comment and the library's own
  `begin()`/`connect()` comments should stay consistent with each other on
  the root-cause story.

## Gotchas not obvious from the code

- **Never commit `firmware/src/secrets.h`.** Gitignored via
  `firmware/.gitignore` (`src/secrets.h`, relative to that file). Contains
  a real WiFi password and a Claude Code OAuth token (a personal bearer
  credential sent with `anthropic-beta: oauth-2025-04-20`). Only
  `secrets.example.h` (placeholder values) is meant to be committed.
- `firmware/src/certs.cpp` is a **public** CA bundle (GlobalSign Root CA,
  ISRG Root X1, DigiCert Global Root G2) for `api.anthropic.com` TLS - not
  sensitive, just there so a server-side CA rotation doesn't need a
  firmware update before ~2028/2035/2038 (expiry dates in `certs.h`'s
  comment).
- ESP32-C3 SuperMini clones need `-DARDUINO_USB_MODE=1
  -DARDUINO_USB_CDC_ON_BOOT=1` (both, together) in `platformio.ini` for a
  usable Serial port over the native USB-Serial-JTAG peripheral - already
  set; removing either one silently breaks Serial on this class of board.
- If the LCD stays blank/garbled after flashing, it's almost always the
  contrast potentiometer on the back of the PCF8574 backpack, not a code
  or wiring bug - it ships turned all the way down on most modules.
- `firmware/.pio/` (PlatformIO's build cache) is gitignored; don't
  hand-clean it, `pio run -t clean` does that.
- `REFRESH_INTERVAL_MS` (120s) hits the real Anthropic Messages API every
  poll - don't lower this aggressively, it's a real (if minimal,
  `max_tokens=1`) request against production rate limits.
- `PIN_SCREEN_BUTTON` is GPIO7. GPIO6 was tried first (also chosen to
  avoid ESP32-C3's strapping pins GPIO2/8/9 - GPIO9 in particular is the
  same pin as the BOOT button on most boards) but didn't work on this
  device's actual board for reasons not root-caused (could be a
  board-specific pinout difference, could be wiring - not confirmed
  either way); switched to GPIO7 on the user's instruction rather than
  debugging further. If GPIO7 ever needs to change again, GPIO3 or GPIO10
  are the next things to try - avoid SDA/SCL and the strapping pins.

## Naming note

The repo is named `claude-desktop-display`; the project's own
`README.md`/LCD banner call it "ClaudeMeter LCD" (chosen for its
ClaudeMeter-macOS-app lineage - see Credits in `README.md`). Both names
refer to the same project; this file and the README use them
interchangeably.

## Non-goals / out of scope

- Not a general Claude Code dashboard - only surfaces the two utilization
  numbers Claude Code itself already checks (5h/7d), nothing else from the
  API.
- No AP/provisioning UI, no OTA - WiFi credentials and the OAuth token are
  compiled into the firmware via `secrets.h`, matching
  esp32-wifi-fix-kit's own scope boundary.
