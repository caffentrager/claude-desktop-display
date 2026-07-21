# readai.md — context for an AI assistant picking this up cold

## What this is

**Claude Desktop Display**: an ESP32-C3 + I2C character-LCD desk gadget that shows
Claude Code's 5-hour and weekly rate-limit usage, polling
`api.anthropic.com` directly with a Claude Code OAuth token (`claude
setup-token`) - no PC has to stay running. See `README.md` for user-facing
setup/wiring/troubleshooting.

- Repo name is `claude-desktop-display`, matching the project's own name
  and the actual boot-splash text (see "Naming note" below).
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

## Display: 2 active screens (5H, WK) + a held combined screen, button-switchable

`SCREEN_COUNT` is 2 as of 2026-07-21 - the combined-glance screen (see
below) is fully implemented but disabled per explicit user request
("일단 보류" - hold for now, not deleted). Bump `SCREEN_COUNT` back to 3
to bring it back; nothing else needs to change.

The LCD cycles through `SCREEN_COUNT` screens (`currentScreen`), advancing
every `SCREEN_ROTATE_MS` from `loop()` **or** on a press of
`PIN_SCREEN_BUTTON` (debounced in `checkScreenButton()`, standard Arduino
debounce pattern - raw reading must sit still for `BUTTON_DEBOUNCE_MS`
before it counts). A button press also resets `lastScreenSwitchMs`, so the
automatic timer doesn't immediately advance again right on top of a
manual one.

**Bug history worth knowing:** an earlier version of `loop()` ended in
`delay(1000)`, and `checkScreenButton()` (called once per iteration) was
mis-documented here as having "up to ~1s latency... a deliberate
non-goal, not a bug." That was wrong - it wasn't just latency, it was
actual **missed presses**: any tap shorter than the ~1s window between
`digitalRead()` samples (i.e., basically any normal human tap) could land
entirely between two samples and never be seen at all, which is almost
certainly the real reason the user reported "버튼이 잘 안 먹음" (button
doesn't respond well). Fixed by changing the loop's trailing `delay(1000)`
to `delay(20)` - everything else in `loop()` (fetch interval, staleness,
screen rotation) was already gated on its own `millis()` comparison, not
on "how many loop iterations passed," so shortening the delay only speeds
up button polling; it doesn't need a companion "run this every 1s
instead" timer for the rest, and `render()`'s own I2C change-detection
means calling it ~50x more often than before doesn't add real I2C
traffic. **If a button issue ever comes back, check this class of bug
before assuming it's the GPIO pin.**

Screens 0/1 ("5H"/"WK" detail, designed with `lcd_editor.html` - see
below) are each 2 rows:

- Row 0: label + a `BAR_WIDTH`-cell bar graph of the percentage, drawn
  with `drawBar()`, plus a decorative sparkle (`LOGO_CHAR`). `BAR_WIDTH`
  is `LCD_COLS - 5` (not a fixed number) so `"<label> <bar> <sparkle>"`
  (2 + 1 + BAR_WIDTH + 1 + 1) always totals exactly `LCD_COLS` - the bar
  fills whatever's left over instead of leaving unused columns, whatever
  LCD_COLS is set to.
- Row 1: the exact percentage as a number, plus a time detail, plus the
  same sparkle again at the very end *if it fits* (`render()` measures
  `strlen(row1)` and only appends `LOGO_CHAR` (plus 2 leading spaces on
  screen 1 only, 0 on screen 0 - see the CGRAM slot-5 note below) when it
  fits - on this device's 18-col LCD it always does, exactly: screen 0's
  worst case "100% 6h 59m Left!" is 17 chars + 0 gap + 1 logo = 18;
  screen 1's worst case "100% Fri 19:00!" is 15 chars + 2 gap + 1 logo =
  18. This degrades gracefully instead of truncating real content on a
  narrower `LCD_COLS`, though):
  - Screen 0 ("5H"): `formatCountdown()` - a relative "3h 5m" countdown to
    when the 5-hour window resets (lowercase with a space, not "3H5M" -
    reads more like ordinary text; minutes still aren't zero-padded -
    "3h 5m" not "3h 05m", changed on request since the leading zero read
    as harder to scan; under an hour left, the hour part is dropped
    entirely - "45m", not "0h 45m" - rather than showing a zero hour
    count; screen 0's `render()` branch appends " Left" itself;
    `formatCountdown()`'s raw output has no suffix so screen 2 can reuse
    it without one - see below). Relative reads better for a short
    window.
  - Screen 1 ("WK"): `formatResetDay()` - an absolute "Fri 19:00" (weekday
    + time, shifted by `DISPLAY_TZ_OFFSET_SEC`) for when the 7-day window
    resets. Absolute reads better than a multi-day countdown.
    `DISPLAY_TZ_OFFSET_SEC` is hardcoded to `9*3600` (KST, this device's
    desk) per explicit user request rather than made configurable/derived
    from anything - only shifts the *displayed* value passed to
    `gmtime_r()`, not `resetEpoch` itself, so it can't skew
    `formatCountdown()`'s duration math (which stays pure UTC-vs-UTC).

Screen 2 (`renderCombinedScreen()`, currently unreachable - see
`SCREEN_COUNT` above) shows both metrics on one screen, no rotation
needed for a quick glance: `"5H<pct> <countdown>"` /
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

CGRAM (8 slots, 0-7) has 1 free slot (7). The full allocation history is
worth knowing since it moved around twice in one session:
- 0: `FULL_BLOCK_CHAR`. Briefly moved to the HD44780 ROM's built-in solid
  block (`0xFF`, no CGRAM slot needed on ROM code A00 - what the large
  majority of PCF8574-backed character LCDs ship with) to free up a slot,
  then moved *back* to CGRAM once the bar cells got the rows-1/6 inset
  redesign (below) - a ROM glyph's bit pattern can't be customized, so
  "free a slot" and "give every cell state a consistent look" turned out
  to be mutually exclusive, and consistent look won on request. If this
  is ever ported to a board with a non-A00 ROM variant (A02, European, is
  the other common one), `0xFF` may not be a solid block there - not a
  concern now since it's back in CGRAM, but relevant if anyone reverts
  this again.
- 1-4: partial-fill glyphs (`PARTIAL_FILL_CHARS`, 1-4 of `BAR_SUBDIV`=5
  columns lit) - only the one bar cell straddling the fill boundary ever
  uses one of these; `drawBar()` computes `filledUnits` in units of
  1/`BAR_SUBDIV` of a cell rather than whole cells, for
  `BAR_WIDTH * BAR_SUBDIV` distinct bar levels instead of just `BAR_WIDTH`.
  Rows 0 and 7 are full-width (`0b11111`), matching `FULL_BLOCK_CHAR` and
  `EMPTY_MID_CHAR`, so the bar's top/bottom border runs continuously
  across every cell regardless of fill state. Rows 1 and 6 are always
  blank (a 1px inset gap just inside that border), with the actual fill
  columns confined to rows 2-5 - a "recessed/framed" look requested to
  read more like a loading bar.
- 5: `LOGO_CHAR`, a small original sparkle/starburst (not a reproduction
  of any real logo - 5x8 monochrome pixels can't meaningfully do that
  anyway), shown on the boot splash (`"<sparkle> Claude"` / `"Desktop
  Display"`, two rows, in `setup()` before WiFi connects), at the end of
  row 0 on screens 0/1 (always fits, since `BAR_WIDTH` is sized to leave
  exactly enough room for it), and at the end of row 1 too if there's
  space left over (see the row 1 note above) - attached directly (no
  gap) on screen 0, 2 cells of gap on screen 1 (widened from 1 on
  request), so the two screens don't look identical in how tightly it
  sits against the preceding text.
- 6: `EMPTY_MID_CHAR` - just a top/bottom line, no sides. **Used for
  every empty bar cell uniformly, including the first and last cells -
  no special bracket-shaped end-cap.** This went through two redesigns in
  one session: a full hollow-rectangle outline (top+bottom+both sides)
  on every empty cell was judged too heavy, then a version with a
  dedicated bracket-shaped end-cap glyph (`EMPTY_END_CHAR`, a mirrored
  Korean "ㄷ"/"⊐" shape, used only for `i == BAR_WIDTH - 1`) was tried for
  visual symmetry with the bar's edges - but adding a matching *start*-cap
  for the first cell too would have needed a 9th CGRAM slot (only 8
  exist), forcing a tradeoff against the logo, `BAR_SUBDIV` resolution, or
  this glyph's own border continuity. Asked the user, who instead chose
  the simplest option not on the original list: drop the dedicated
  end-cap entirely and use this same top/bottom-only glyph everywhere,
  first/middle/last cells alike. `EMPTY_END_CHAR` and its `emptyEndGlyph`
  array no longer exist in the code - don't reintroduce them without
  re-confirming, since this was an explicit simplification, not an
  oversight.
- 7: free.

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
- `PIN_SCREEN_BUTTON` history: GPIO6 → GPIO7 → GPIO20, in that order, all
  within 2026-07-21. GPIO6 and GPIO7 were each reported as "doesn't work
  well" and swapped out without root-causing why at the time - **the real
  bug turned out to be unrelated to the pin choice** (see the `loop()`
  polling-rate bug in the Display section above: any of these three pins
  would have shown the same symptom, since the actual problem was
  `checkScreenButton()` only being sampled once a second). GPIO20 is what
  shipped. `PIN_DEBUG_BUTTON` (GPIO21, added same day) shares the same
  debounce pattern in its own `checkDebugButton()`/state variables rather
  than reusing `checkScreenButton()`'s - simple duplication rather than a
  shared helper, since there are only two of these and they trigger
  different actions. Both GPIO20/21 are normally UART0 RX/TX - fine here
  since Serial goes over native USB-CDC instead (`ARDUINO_USB_MODE=1` in
  `platformio.ini`), but worth knowing if this code is ever adapted for a
  board/config that uses the classic UART0. If either ever needs to
  change again, prefer another free, non-strapping, non-SDA/SCL pin
  (GPIO2/8/9 are ESP32-C3's strapping pins).
- `runDebugTest()` (triggered by `PIN_DEBUG_BUTTON`) is a blocking ~25s,
  4-phase self-test:
  1. Sweeps `drawBar()` through percent 0-99 (120ms/step), lingering 3s
     on the p==0 frame first so the all-empty state is actually visible
     before the sweep starts moving rather than flashing by at the same
     pace as every other step.
  2. Cycles all 7 weekday abbreviations through the same `"%s HH:MM"`
     format `formatResetDay()` uses (hardcoded to a fixed time, not run
     through `formatResetDay()` itself, since driving that function
     through all 7 weekdays would need 7 fake epoch values - simpler to
     just test the display format directly).
  3. Sweeps the clock display `"%02d:00"` from hour 0 through 24
     inclusive (150ms/step) - 24 is included deliberately as a
     display-sweep boundary (checking the two-digit format renders for
     every value), not a claim the device ever shows a real "24:00".
  4. Sweeps `"00:%02d"` from minute 0 through 59 (100ms/step), hour held
     at 0 - phase 3 only ever showed ":00", so this is the other half of
     the two-digit range that phase needed but didn't cover.

  Exists so the full range of all four can be eyeballed on the real
  hardware without waiting for live usage data or the real calendar/clock
  to naturally pass through every value. Ends with `forceRedraw()` so
  normal operation resumes cleanly.

## Naming note

The project is called **Claude Desktop Display**, matching the repo name
(`claude-desktop-display`) and the actual boot-splash text in `setup()`
(`"<sparkle> Claude"` / `"Desktop Display"`). Earlier drafts of these docs
also called it "ClaudeMeter LCD," as a nod to
[eddmann/ClaudeMeter](https://github.com/eddmann/ClaudeMeter) (the macOS
app that originally inspired this project - still credited in Credits in
`README.md`). That alternate name has since been dropped everywhere except
that one Credits attribution, so there's a single name for the project
itself now, not two used interchangeably.

## Non-goals / out of scope

- Not a general Claude Code dashboard - only surfaces the two utilization
  numbers Claude Code itself already checks (5h/7d), nothing else from the
  API.
- No AP/provisioning UI, no OTA - WiFi credentials and the OAuth token are
  compiled into the firmware via `secrets.h`, matching
  esp32-wifi-fix-kit's own scope boundary.
