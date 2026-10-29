# AGENTS.md — ESP32-SIP-Voice

ESP-IDF (v6.1) firmware for an ESP32/ESP32-S3 SIP VoIP phone with a web UI,
optional LED matrix display, LVGL TFT UI, I2S audio (G.711/G.722/OPUS), and
AEC. Targets: ESP32, ESP32-S2, ESP32-S3, ESP32-C3. Profile tiers (LITE /
STANDARD / PRO) are selected in `main/Kconfig.projbuild`.

## Build / flash / monitor

```powershell
# Activate IDF (Windows)
& C:\esp\v6.1\esp-idf\export.ps1

# Configure target (only needed once / when switching chips)
idf.py set-target esp32s3          # or esp32, esp32c3, esp32s2

# Build
idf.py build

# Flash (replace COM8 with the actual port)
idf.py -p COM8 flash

# Monitor
idf.py -p COM8 monitor
```

- Default flash size for the S3 speaker build is **16 MB** (`sdkconfig` /
  `sdkconfig.defaults`). The 4 MB partition table in `partitions.csv` is for
  classic ESP32 boards.
- `managed_components/`, `build/`, `sdkconfig`, `sdkconfig.old`,
  `dependencies.lock` are git-ignored. Do not commit them.
- `.gitignore` must be UTF-8 (a UTF-16 `.gitignore` is silently ignored by git).

## Project layout

```
main/                      app component
  main.c                   boot, init order, SIP<->LVGL bridges, SNTP
  sip_client.{c,h}         SIP stack (REGISTER/INVITE/BYE/CANCEL/INFO, digest, STUN, SDP)
  rtp_handler.c            RTP send/recv
  audio_pipeline.c         I2S task, codec dispatch, AEC, wake word
  g711_codec.c             G.711 encode/decode
  codec_driver.{c,h}       audio backend selection (I2S amp vs I2C codec) + volume
  wifi_manager.c           STA/AP, captive portal, retry/backoff
  net_manager.c            network mode (Wi-Fi vs ENC28J60 Ethernet), AP detection
  ui_controller.{c,h}      HTTP server: login, settings, hardware, phonebook, call control
  display.c                SSD1306 OLED driver
  matrix_status.{c,h}      LED matrix status renderer task
  led_status.{c,h}         onboard RGB LED status (S3 only)
components/
  config_store/            app_config.h (compile-time defaults) + config_manager (NVS)
  audio_pipeline/          G.722, OPUS, NLMS AEC, WakeNet
  board_hal/               TFT panels, touch, keypads
  matrix_display/          HUB75 / MAX7219 / preview backends
  storage/                 NVS phonebook
  ui_lvgl/                 LVGL screens (3 themes; also builds for PC simulator)
simulator/                 SDL2 LVGL preview (SIM_THEME, SIM_ROUND)
docs/                      FEATURE-PLAN.md, HUB12-32x16-PLAN.md, AUDIT.md
tools/                     MAX7219 test sketches, esp-mcp
```

## Init order (main.c, do not reorder casually)

1. NVS + config load (`config_manager_load`)
2. Phonebook init
3. HAL: keypad, TFT, touch
4. LVGL init + display init
5. LED matrix init (optional, from `build_matrix_cfg`)
6. Event group + TCP/IP + net_manager (Wi-Fi/Ethernet, captive portal on missing creds)
7. If AP mode: `ui_controller_init(NULL, ...)` and return (SIP disabled)
8. Audio pipeline init
9. SIP client init
10. UI controller init (with SIP handle)
11. SIP <-> LVGL callback wiring
12. Wake-word callback wiring
13. SNTP
14. `app_control_task` (state machine)
15. `ota_mark_valid()`

`ui_controller_init` is called **twice** on purpose: once in AP mode (NULL SIP
handle) and once in normal mode. Do not remove the first call.

## Concurrency rules (critical)

- **The SIP task owns the socket.** Never build or send SIP messages from the
  HTTP server, button task, timer task, or LVGL task — they only have 2–4 KB
  stacks and will overflow. Use the deferred-action pattern in `sip_client.c`:
  public entry points (`sip_client_initiate_call`, `_answer_call`,
  `_terminate_call`) only set `pending_action`/`pending_target` under
  `action_lock`; `sip_task` runs `service_pending_action()` every loop.
- `sip_client_reload()` (web settings save) sets `reload_pending`; applied by
  `sip_task` only when `call_state == IDLE`, with DNS backoff.
- `registration_timer_callback` runs in the FreeRTOS timer task (2 KB stack):
  it must only queue `SIP_ACTION_REGISTER`, never send.
- Matrix drawing happens only in the matrix task (`matrix_status_task`); callers
  set state. Same for the RGB LED task.
- Audio pipeline runs in its own task at priority 10; SIP at 8; matrix at 3.
- Settings race: HTTP task mutates `*g_settings`; consumers must read a snapshot,
  not the live pointer, when consistency matters.
- `app_control_task` must `vTaskDelay` in its loop (bits already set return
  immediately from `xEventGroupWaitBits`) — otherwise it spins at prio 6 and
  starves the idle task (task-watchdog spam).

## SIP client deferred-action enum

`sip_action_t`: `SIP_ACTION_NONE`, `INVITE`, `ANSWER`, `HANGUP`, `REGISTER`.
`client->pending_action` (uint8) + `pending_target[128]`, guarded by
`portMUX_TYPE action_lock`.

## Web UI (`ui_controller.c`)

- All pages share one inline `PAGE_CSS` (light theme ported from
  `page-design.html`). No external assets, no JavaScript.
- `page_t` buffer grows on demand (1 KB → 24 KB cap) via `pg_reserve`.
- Auth: cookie-based session (`ESPAUTH`, 8 h sliding, HttpOnly/SameSite=Lax).
  `/login` GET/POST, `/logout`. `require_login()` guards every page; the
  captive-portal catch-all `/*` redirects unauthenticated users to `/login`.
- Login throttle: 5 failures → 30 s lockout, tracked with timestamps (no
  blocking sleep in the HTTP task).
- Web credentials are **not** editable from the open setup AP (lockout risk).
- `apply_str()` returns `APPLY_UNCHANGED / APPLY_CHANGED / APPLY_TOO_LONG`;
  over-length values are rejected, never truncated. Blank password fields mean
  "keep stored value".
- Tabs: `/setup?tab=...` and `/hardware?tab=...`, server-rendered, no JS.
  Unknown tab falls back to the first.
- `setup_post_handler` groups changes into `wifi_changed / sip_changed /
  web_changed / mode_changed / audio_changed / volume_changed`; only Wi-Fi,
  audio backend and AP mode force a reboot; everything else applies live.
- `sip_client_reload()` re-registers without reboot (de-reg on old server addr,
  DNS backoff 5→60 s).
- Hardware POST ignores blank pin fields (a blank must not become pin 0).

## Config / NVS (`config_store/`)

- `app_config.h`: compile-time defaults (`WIFI_*`, `SIP_*`, `WEB_UI_USER/PASSWORD`,
  pin maps per target, `USE_CODEC_*`, `CTRL_METHOD_*`, `APP_VERSION`).
- `app_settings_t`: Wi-Fi, SIP (server/port/domain/user/pass/name/target),
  device role, auto-answer delay, audio output, volume, display/matrix settings.
- `config_manager_load/save`: per-key NVS read with compile-time fallback;
  `apply_defaults()` seeds every field. New keys added must be handled in both
  load and save.
- `hardware_settings_t`: GPIO map + UI theme, persisted as a blob (`hw_config`).

## Audio

- `audio_pipeline_init()` creates the I2S task. TX-only mode when no mic pin
  (`din == -1`); full-duplex otherwise.
- Codec backend chosen at runtime from `audio_out` setting (Auto / I2S amp /
  I2C codec). MAX98357A has no volume register → software gain via
  `codec_apply_volume()` before the AEC reference copy.
- I2S TX is muted while idle (no repeating DMA frames / "woodpecker"); rate
  changes tolerate a muted channel.
- AEC reference = last played frame.

## Display / matrix

- Backends: HUB75 (stub, pending driver), MAX7219 (working, CS latched per
  row, bit 7 = leftmost), Preview (ASCII).
- `matrix_status_task` renders state at 10 Hz; multi-line independent slots;
  screensaver rotates effects after idle timeout.
- MAX7219 chain on this S3 build runs right-to-left (`CHAIN_REVERSED`); the
  firmware maps `x=0` to the farthest module.
- Onboard RGB LED (GPIO 48, WS2812 via RMT) mirrors state.

## Default GPIO map (compile-time fallbacks in `app_config.h`)

All pins are overridable at runtime via the web Hardware page (NVS). `-1` means
"not wired". The S3 map reuses the (otherwise unused) keypad GPIOs 10–17.

### ESP32-S3 (primary speaker build)

| Function | GPIO |
|:--|:--:|
| I2S BCLK / WS / DOUT / DIN | 5 / 7 / 6 / 4 |
| Codec I2C SCL / SDA | 8 / 9 |
| Keypad rows 1-4 / cols 1-4 | 10-13 / 14-17 |
| TFT MOSI / SCLK / CS / DC / RST | 47 / 48 / 21 / 40 / 41 |
| Touch CS / IRQ | 18 / 38 |
| TFT MISO | -1 |
| MAX7219 DIN / CLK / CS | 10 / 11 / 12 (reuses keypad rows!) |
| MX HUB75 R1..B2 / A,B,C,D,E / CLK,LAT,OE | 10-15 / 16,17,18,21,-1 / 1,2,42 |
| ENC28J60 SCK/MOSI/MISO/CS/INT/RST | 47 / 48 / 21 / 3 / 14 / 45 |
| Button | 0 |
| Onboard RGB LED (WS2812) | 48 |

### HUB12 display (32x16, P10) default pins (ESP32-S3 only)

The HUB12 panel uses 12 pins (a subset of the 14-pin HUB75 map — no C/D/E).
These are the compile-time defaults; all are overridable via the web Hardware
page. The HUB12 backend shares the same pin array as HUB75, with C/D/E left
at -1.

| Signal | GPIO | Notes |
|:--|:--:|:--|
| R1 / G1 / B1 | 10 / 11 / 12 | upper half RGB |
| R2 / G2 / B2 | 13 / 14 / 15 | lower half RGB |
| A / B | 16 / 17 | row address (1/4 scan) |
| CLK | 1 | shift clock |
| LAT | 2 | latch |
| OE | 42 | output enable (active low) |
| C / D / E | -1 | unused on 1/4-scan panels |
| GND | common | must share ground with ESP32 |
| VCC | 5 V | panel supply (bulk cap recommended) |

> Note: these reuse the MAX7219 ticker pins (10/11/12) and the keypad rows.
> Only one of MAX7219 / HUB12 / keypad can own those pins at a time. Move the
> MAX7219 pins to a free set (e.g. 35-37) for dual-panel use.

### ESP32-C3

| Function | GPIO |
|:--|:--:|
| I2S BCLK / WS / DOUT / DIN | 4 / 5 / 6 / 7 |
| Codec I2C SCL / SDA | 8 / 9 |
| Keypad rows 1-4 / cols 1-4 | 0-3 / 10-11,18-19 |
| TFT MOSI / SCLK / CS / DC / RST | 6 / 7 / 10 / 20 / 21 |
| Touch CS / IRQ | 1 / 0 |
| TFT MISO | -1 |

### ESP32 (classic)

| Function | GPIO |
|:--|:--:|
| I2S BCLK / WS / DOUT / DIN | 26 / 25 / 22 / 21 |
| Codec I2C SCL / SDA | 18 / 19 |
| Keypad rows 1-4 / cols 1-4 | 32,33,27,14 / 12,13,4,5 |
| TFT MOSI / SCLK / CS / DC / RST | 23 / 18 / 15 / 2 / 4 |
| Touch CS / IRQ | 14 / 27 |
| TFT MISO | -1 |
| MAX7219 DIN / CLK / CS | 32 / 33 / 27 |

> Note: on the S3 build the MAX7219 ticker pins (10/11/12) collide with the
> HUB75 R1/G1/B1 pins and the keypad rows. Only one of MAX7219 / HUB75 /
> keypad can use those pins at a time; pick via the Hardware page or move the
> MAX7219 pins to a free set (e.g. 35-37) for dual-panel use.

## Coding conventions

- Language: C (ESP-IDF), C++ only for the HUB75 backend shim.
- No comments unless explicitly requested.
- 4-space indentation, K&R braces.
- All user-facing strings HTML-escaped via `pg_escape`.
- Stack budget: SIP task 10 KB, HTTP server 6 KB, audio task, matrix task 4 KB.
- Never sleep in the HTTP task; use timestamp-based throttling.

## Testing

- Build: `idf.py build` (must exit 0, no new warnings).
- Flash: `idf.py -p COMx flash`.
- Web UI previews: `preview_*.html` mirror the firmware markup.
- Serial monitor for boot logs.

## Known open items

- HUB75 driver (esphome/esp-hub75) does not build on IDF 6.1; backend is a
  stub.
- SRTP and SIPS/TLS incomplete.
- OPUS requires adding libopus to `components/audio_pipeline/idf_component.yml`.
- No physical volume control (web only).
EOF
echo "AGENTS.md written"