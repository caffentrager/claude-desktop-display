[English](README.md) | [한국어](README.ko.md)

# ClaudeMeter LCD

A standalone desk gadget that shows your Claude **5-hour** and **Week** rate-limit
usage on a character LCD wired to an ESP32-C3. No PC has to stay running -
the device talks to Anthropic's API directly.

```
 ESP32-C3  <--HTTPS, OAuth Bearer token-->  api.anthropic.com
(SDA=GPIO4, SCL=GPIO5)                      (real Messages endpoint)
```

It works the same way the [claude-usage-stick](https://github.com/oauramos/claude-usage-stick)
project (and the Claude Code CLI itself) check your limits: send a tiny
`max_tokens: 1` request to `/v1/messages` using your Claude Code OAuth
token, then read the utilization straight out of the response headers -
`anthropic-ratelimit-unified-5h-utilization` and
`anthropic-ratelimit-unified-7d-utilization`. That's the real API endpoint,
so there's no Cloudflare bot-detection to fight and no browser cookie to
extract, unlike scraping claude.ai's internal web API (which is what the
original [ClaudeMeter](https://github.com/eddmann/ClaudeMeter) macOS app,
this project's original inspiration, does).

## Hardware

- An ESP32-C3 board (any variant - DevKitM-1, "SuperMini", Xiao ESP32C3, ...)
- A character LCD (16x2 or 20x4) with a **PCF8574 I2C backpack** (the common
  4-pin GND/VCC/SDA/SCL modules)
- 4 jumper wires

### Wiring

| LCD backpack pin | ESP32-C3 pin |
|---|---|
| GND | GND |
| VCC | 5V (or 3V3 - see note below) |
| SDA | GPIO4 |
| SCL | GPIO5 |

> Most PCF8574 backpacks and the HD44780 LCD they drive want 5V for a bright
> backlight/contrast; the I2C bus itself is fine being read by the ESP32-C3's
> 3.3V-logic pins alongside a 5V-powered backpack in the vast majority of
> these modules (no logic-level shifter needed in practice). If your board
> doesn't expose a 5V pin (USB Vbus passthrough), 3V3 works too, just dimmer.

If nothing shows up on screen, turn the small potentiometer on the back of
the LCD/backpack - that's the contrast adjustment, and it's often turned all
the way down out of the box.

## Setup

### 1. Get a Claude Code OAuth token

On any machine where you have the [Claude Code CLI](https://docs.anthropic.com/en/docs/claude-code)
installed and logged in:

```bash
claude setup-token
```

This prints a long-lived OAuth token meant for exactly this kind of
non-interactive use. Copy it.

### 2. Configure and flash the firmware

Requires [PlatformIO](https://platformio.org/) (CLI or the VS Code extension).

```bash
cd firmware
cp src/secrets.example.h src/secrets.h
# edit src/secrets.h: WiFi SSID/password + the token from step 1
pio run --target upload
pio device monitor
```

The serial monitor shows the LCD's detected I2C address, WiFi connection
progress, and each poll's HTTP status. If your LCD is 20x4 instead of 16x2,
change `LCD_COLS` / `LCD_ROWS` at the top of `firmware/src/main.cpp` - each
row's text is short and left-aligned either way, so a wider display just
shows it with extra blank space on the right rather than needing layout
changes.

That's it - no bridge script, no second machine to keep running.

## Reading the display

The screen alternates between two views every few seconds
(`SCREEN_ROTATE_MS` in `main.cpp`), each a label + 10-cell usage bar on
row 1, and the exact percentage + a time detail on row 2:

```
5H [███░░░░░░░]
42% 3H12M Left
```

```
WK [██████░░░░]
67% Fri 19:00
```

- **5H** - utilization of your rolling 5-hour session limit, with a
  countdown to when it resets
- **WK** - utilization of your rolling 7-day (week) limit, with the
  weekday + time it resets (a countdown in hours is less readable for a
  multi-day window). Shown in `DISPLAY_TZ_OFFSET_SEC`'s timezone in
  `main.cpp` - hardcoded to KST (UTC+9) for this build; change that
  constant if you're building for a different timezone.
- A trailing `!` on row 2 means the device hasn't gotten a good reading in
  a while (WiFi hiccup, API timeout, etc.) - the last known values stay on
  screen.
- The time detail reads `--` if the device hasn't yet fetched a valid
  reset time, or (5H screen only) hasn't finished syncing its clock over
  NTP yet (a few seconds after boot/reconnect - see Troubleshooting).
- **"Auth failed! / Redo setup-token"** replaces both screens if Anthropic
  returns 401 - your token has expired or was revoked. Run `claude
  setup-token` again and update `secrets.h`.

Polling happens every 2 minutes by default (`REFRESH_INTERVAL_MS` in
`main.cpp`) since each check is a real, though minimal, API call - there's
no need to hit it every few seconds.

## Layout tool

[`lcd_editor.html`](lcd_editor.html) is a standalone, offline layout
designer used to mock up the screens above - open it in any browser,
type directly into the 18x2 grid of cells, and it generates the matching
`lcd.setCursor(col, row)` / `lcd.print("...")` calls for whatever you laid
out. No build step, no dependencies.

## Troubleshooting

- **Blank/garbled LCD** - adjust the contrast pot on the backpack; also
  check the Serial monitor log for the detected I2C address (some
  backpacks use `0x3F` instead of the common `0x27` - the firmware
  auto-scans for whatever's on the bus, so this is just for your own
  sanity check).
- **Stuck on "Connecting WiFi"** - ESP32-C3 only does 2.4GHz WiFi; make
  sure you're not pointing it at a 5GHz-only SSID.
- **Stuck showing "DISCONN r=2" (AUTH_EXPIRE) on every attempt, correct
  password, network visible in scan** - some ESP32-C3 SuperMini/clone
  boards have a real antenna-impedance-matching defect that reflects the
  default TX power and corrupts the auth handshake, identically regardless
  of which AP it's talking to
  ([arduino-esp32 #6767](https://github.com/espressif/arduino-esp32/issues/6767)).
  The firmware already works around this
  (`WiFi.setTxPower(WIFI_POWER_8_5dBm)` right after `WiFi.begin()` in
  `connectWiFi()`), so you shouldn't need to do anything - but if you're
  modifying the WiFi code and reintroduce this symptom, that's why.
- **"Auth failed!" on screen / HTTP 401 in the serial log** - your OAuth
  token expired or was revoked. Re-run `claude setup-token`, update
  `secrets.h`, and reflash.
- **Stays stale (`!`) with HTTP errors other than 401 in the log** - check
  the ESP32 actually has internet access (not just LAN).
- **5H screen's countdown shows `--` instead of a time** - it needs the
  device's own clock, synced over NTP (`configTime()` in `setup()`);
  this takes a few seconds after WiFi comes up, and briefly shows `--`
  until it completes. If it never clears, check the ESP32 has real
  internet access (NTP needs UDP/123 outbound, not just HTTP(S)). (The WK
  screen's reset day/time doesn't need this - it's computed straight from
  the API's own reset timestamp, no local clock involved.)
- **Either screen's time detail is stuck on `--` even though the
  percentage is populated** - the `anthropic-ratelimit-unified-{5h,7d}-reset`
  headers didn't come back, or came back in an unexpected format. Check the
  Serial log line `[API] 5h=... reset5h='...' 7d=... reset7d='...'` in
  `fetchUsage()` - as of 2026-07-21 these are plain Unix timestamps (e.g.
  `1784652600`), parsed by `parseResetEpoch()` in `main.cpp`.

## Security note

Unlike claude-usage-stick (which PIN-encrypts the token at rest since it's
built for boards with buttons), this project stores your OAuth token in
plain text in `secrets.h`/flash - the same way it already stores your WiFi
password. That's a reasonable tradeoff for a personal device sitting on
your own desk, but anyone with physical/USB access to the board or the
compiled firmware could extract it. If that matters to you, look at
claude-usage-stick's `crypto.cpp` (AES-256-GCM + PIN) for a starting point -
it just needs buttons for PIN entry, which this build doesn't wire up.

## Disclaimer

This is an unofficial, community project, not affiliated with or endorsed
by Anthropic. It reuses your personal Claude Code OAuth token outside the
Claude Code client to poll the same rate-limit headers Claude Code itself
reads - Anthropic could change or restrict this at any time. Use at your
own risk. Your token only ever goes from this device directly to
`api.anthropic.com`; nothing is sent to any third party.

## Credits

- [eddmann/ClaudeMeter](https://github.com/eddmann/ClaudeMeter) - original
  inspiration (macOS menu bar app; this project started as a hardware
  companion to it before switching to a direct-API approach)
- [oauramos/claude-usage-stick](https://github.com/oauramos/claude-usage-stick) -
  the OAuth + rate-limit-header technique and CA bundle used here (MIT)
- [caffentrager/esp32-wifi-fix-kit](https://github.com/caffentrager/esp32-wifi-fix-kit) -
  the `Esp32WifiFix` library (`firmware/lib/Esp32WifiFix/`) used here for
  stale-NVS-cache clearing and defensive HT20 bandwidth; this project's own
  WiFi debugging (see [readai.md](readai.md)) is in fact what corrected
  that kit's AUTH_EXPIRE root-cause writeup to the antenna-defect
  explanation referenced above (MIT)

## License

MIT.
