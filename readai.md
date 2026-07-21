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
- LCD is a 16x2/20x4 HD44780 over a PCF8574 I2C backpack; firmware
  auto-scans the I2C bus for the backpack's address (`scanForLcdAddress()`
  in `main.cpp`) rather than hardcoding `0x27` vs `0x3F`.
- `render()` only touches the I2C bus when something actually changed.
  Row 0 (label + bar) is tracked by a small change-detection key
  (`renderedRow0Key`) rather than literal text, since the bar's on-screen
  bytes (a mix of a custom CGRAM character and spaces) aren't representable
  as a normal C string; row 1 is plain text, tracked in `renderedRow1` the
  straightforward way.

## Display: rotating 5H/WK screens, each with a bar + a time detail

The LCD alternates between two screens (`currentScreen` in `main.cpp`,
flipped every `SCREEN_ROTATE_MS` from `loop()`) - designed with
`lcd_editor.html` (see below). Each screen is 2 rows:

- Row 0: `"5H"`/`"WK"` label + a fixed `BAR_WIDTH`-cell (10) bar graph of
  the percentage, drawn with `drawBar()` using a custom full-block CGRAM
  glyph (`FULL_BLOCK_CHAR`).
- Row 1: the exact percentage as a number, plus a time detail:
  - Screen 0 ("5H"): `formatCountdown()` - a relative "3H12M Left"
    countdown to when the 5-hour window resets. Relative reads better for
    a short window.
  - Screen 1 ("WK"): `formatResetDay()` - an absolute "Fri 04:00" (UTC
    weekday + time) for when the 7-day window resets. Absolute reads
    better than a multi-day countdown.

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
