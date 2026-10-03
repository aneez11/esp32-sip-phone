# ESP32-SIP-Voice — Matrix Display / Queue / Alert Feature Plan

Status: **planned, not implemented**
Related: [`AUDIT.md`](AUDIT.md), [`../wokwi/README.md`](../wokwi/README.md)

## 0. Decisions (agreed)

| Decision | Choice |
|:--|:--|
| Panel (primary) | HUB75 64×32, 1/16 scan (mono **or** RGB) |
| Panel (secondary) | **8×8 module array, single line** — MAX7219 chain (4 = 32×8, 8 = 64×8), monochrome, 3 SPI pins |
| Target chip | **ESP32-S3** |
| Attachment | **Same ESP32-S3 as the SIP phone** |
| Panel colour | Support **both** monochrome and RGB |
| Queue source | **All four**, selectable in the web UI: MQTT · HTTP · PBX/AMI · local |
| Alert ack | **All methods**, selectable in settings: button · touch · web · timeout |

Two new display roles on top of the existing Phone/Speaker roles:

1. **Caller ID** — show who is calling (name/room resolved from the SIP `From`).
2. **Queue mode** — show the number of waiting callers.
3. **Alert mode** — flash `ROOM 203` (+ optional tone) until acknowledged so a
   person knows which room to go to.

Both panels support a **single-line layout** (an 8-px-tall strip, ideal for a
scrolling caller-ID ticker) as well as a full **2D layout** (queue/alert on the
64×32 HUB75). The active panel is chosen with `matrix_type`; only one panel is
driven at a time.

---

## 1. Hardware

### 1.1 HUB75 panel (primary)

* HUB75 (16-pin), 64×32, **1/16 scan**, A–D address lines (no `E`).
* Colour: RGB panels use R1/G1/B1/R2/G2/B2; monochrome uses only the populated
  channel (usually R1/R2). One backend, `matrix_channels` selects.
* Bring-up checklist: confirm the back label (P5/P4, 1/16 scan, driver IC —
  ICN2037 / DP5125D / RUC7258E / SM5166). esp-hub75 may need
  `shift_driver = FM6126A` instead of `GENERIC` if the panel stays blank.

### 1.2 8×8 single-line array (MAX7219)

An alternative/companion panel: a chain of MAX7219 8×8 modules in **one row**,
used as a single-line ticker (caller ID, queue number, alert text).

* **Chain length:** 4 modules = **32×8**, 8 modules = **64×8**. All modules share
  DIN/CLK/CS and are daisy-chained (DOUT → DIN of the next).
* **Wiring:** 3 signals — `DIN`, `CLK`, `CS` — plus 5 V and GND. Put a 100 nF
  decoupling cap on each module. Logic is 3.3 V-tolerant in practice, but a
  level shifter is still recommended on long cables.
* **Colour:** monochrome (red, green or blue LEDs; `matrix_channels` maps the
  "on" pixels to the populated colour). Brightness via the MAX7219 intensity
  register (0–15 → scaled to 0–100 %).
* **SPI:** use a **dedicated SPI bus (SPI3)** or GPIO bit-bang so matrix refresh
  never contends with the TFT/LVGL SPI2 flush. Slow clock (1–5 MHz) is fine.
* **Single-line rendering:** only 8 px tall → use the 5×7 font at scale 1 (or a
  3×5 font at scale 1/2) with 1 px spacing; content scrolls horizontally.
* **Simulation:** Wokwi emulates the MAX7219 dot-matrix (`wokwi-max7219-matrix`),
  so this backend is fully testable without hardware (see
  [`../wokwi/README.md`](../wokwi/README.md)).
* **Default pins (S3):** `DIN=10`, `CLK=11`, `CS=12` (keypad pins; configurable).

### 1.3 Electrical

* **Level shifter** (74HCT245) between 3.3 V ESP32 and the 5 V HUB75 panel.
* 5 V supply, several amps for RGB (white ≈ 2–3 A), bulk caps (1000 µF/panel).
  The MAX7219 array draws far less (a few hundred mA for 8 modules).
* Common ground with the ESP32.
* Keep the S3 antenna clear — HUB75 DMA is known to interfere with Wi-Fi.

### 1.4 Proposed default S3 pin map

Chosen to avoid I2S (4–7), I2C (8–9), TFT/touch (35–41), USB (19/20), UART
(43/44), flash/PSRAM (26–32) and strapping (0/45/46). It reuses the **keypad
GPIOs 10–17**, which are currently dead code (audit FUN-11).

| Signal | GPIO | | Signal | GPIO |
|:--|:--:|:--|:--|:--:|
| R1 | 10 | | A | 16 |
| G1 | 11 | | B | 17 |
| B1 | 12 | | C | 18 |
| R2 | 13 | | D | 21 |
| G2 | 14 | | CLK | 1 |
| B2 | 15 | | LAT | 2 |
| OE | 42 | | E | –1 (unused, 1/16) |

> If the keypad is ever revived, move the matrix pins; all 14 are runtime
> configurable from the web Hardware page. Defaults live in `app_config.h`
> under a new `#if CONFIG_SIP_MATRIX_HUB75` block.

---

## 2. Driver choice

Use **`esphome/esp-hub75`** (ESP Component Registry, `^0.3.6`) — IDF-native,
DMA-refresh (no CPU in steady state), S3 GDMA path, 1/4–1/32 scan, mono/RGB,
double buffering.

* It is C++; the HUB75 backend is compiled as `matrix_hub75.cpp` behind an
  `extern "C"` wrapper matching the C API in §3.
* Memory: 64×32 8-bit ≈ **28 KB** single-buffer / ~57 KB double (internal SRAM).
* Fallback if it misbehaves: port `mrcodetastic/ESP32-HUB75-MatrixPanel-DMA`.

### 2.1 MAX7219 8×8 single-line backend

* `matrix_max7219.c` — drives a chain of 8×8 modules over SPI (or bit-bang).
* Supports `matrix_width = 8 × modules`, `matrix_height = 8`; the logical
  framebuffer is `modules × 8` bytes (8 bytes per module = one column each).
* Implements the MAX7219 init sequence (decode off, scan all 8 rows, intensity,
  normal operation) and a full-frame flush.
* **Emulated by Wokwi**, so mode/queue/alert logic can be exercised with no
  HUB75 hardware.
* Shares the C API in §3 — the caller does not know which backend is active.

Other backend keeps the logic testable without any panel:

* `matrix_preview.c` — renders to the log and to the web UI as ASCII.

---

## 3. `matrix_display` component

New `components/matrix_display/`:

```
matrix_display.h            C API (backend-agnostic)
matrix_display.c            dispatch, logical framebuffer, font renderer
font5x7.c / font5x7.h       ASCII 5x7 font (scalable 1-4x)
backends/matrix_hub75.cpp   esp-hub75 (C++ -> C shim)
backends/matrix_max7219.c   SPI MAX7219 chain
backends/matrix_preview.c   log + web ASCII preview
idf_component.yml           esphome/esp-hub75, idf
```

C API (sketch):

```c
typedef enum { MX_NONE, MX_HUB75, MX_MAX7219, MX_PREVIEW } matrix_type_t;
typedef enum { MX_MONO_R, MX_MONO_G, MX_MONO_B, MX_RGB } matrix_color_t;
typedef enum { MX_LAYOUT_2D, MX_LAYOUT_LINE } matrix_layout_t; // LINE = 8px ticker

typedef struct {
    matrix_type_t   type;
    matrix_color_t  color;
    matrix_layout_t layout;     // 2D (HUB75) or single-line (MAX7219)
    uint16_t width, height;     // HUB75: 64,32 · MAX7219: 8*modules, 8
    uint8_t  modules;           // MAX7219 chain length (4, 8, ...)
    uint8_t  brightness;        // 0-100
    int8_t   pins[14];          // HUB75: r1..oe · MAX7219: pins[0..2]=din,clk,cs
} matrix_cfg_t;

esp_err_t matrix_init(const matrix_cfg_t *cfg);
void      matrix_clear(void);
void      matrix_set_brightness(uint8_t pct);
void      matrix_draw_text(int x, int y, const char *s, uint8_t scale, uint32_t rgb);
void      matrix_draw_text_center(int y, const char *s, uint8_t scale, uint32_t rgb);
void      matrix_draw_number(int x, int y, int n, uint8_t scale, uint32_t rgb);
void      matrix_scroll_begin(const char *s, uint8_t scale, uint32_t rgb, uint16_t px_per_s);
void      matrix_flush(void);
void      matrix_get_ascii(char *out, size_t out_len); // for web preview
```

* Logical framebuffer: HUB75 64×32×3 ≈ 6 KB; MAX7219 8×8×modules ≈ 64 bytes.
  Text is drawn in the component, then flushed to the backend. Mono mapping:
  any non-black pixel = "on".
* For `MX_LAYOUT_LINE`, the renderer forces scale 1 (5×7 font), vertical-centres
  the text in the 8-px strip and scrolls horizontally; `draw_text_center` is
  ignored (use `matrix_scroll_begin`).
* A dedicated **matrix task** (prio ~3) owns scrolling/animation and the mutex;
  callers only set state (never draw from the SIP task).

---

## 4. `notification_service` (in `main/`)

Consumes the existing SIP callbacks (`main.c:94`, `sip_callbacks_t`) and drives
the matrix. New files `main/notification_service.c/.h`.

```c
void notification_init(app_settings_t *settings, matrix_display_handle_t mx);
void notification_on_incoming(const char *caller_uri, const char *call_id); // from sip_task
void notification_on_answered(const char *call_id);
void notification_on_ended(const char *call_id);
void notification_on_queue(const queue_state_t *q);   // from a queue source
void notification_ack(notif_ack_src_t src);            // button/touch/web/timeout
void notification_tick(void);                          // timeout / re-alert
```

* `on_incoming` is non-blocking: resolves caller→room, sets alert state,
  signals the matrix task.
* Owns alert state machine: `IDLE → RINGING → ACKED → (repeat after
  alert_repeat_s) → timeout`.
* Keeps a small missed-alert log (ring buffer) shown on the web UI.

### Caller → room resolution

Reuse the phonebook: add `bool phonebook_find_by_uri(const char *uri, char *name,
size_t len)` that extracts the SIP user part (`203@…`) and matches entries.
Fallback to the raw caller number. Optional `alert_room_prefix` for display.

---

## 5. Queue sources (implement all, select in UI)

Common model, normalised by every source:

```c
typedef struct { uint16_t waiting; uint32_t longest_wait_s; bool valid; } queue_state_t;
```

| Source | Transport | Implementation | Notes |
|:--|:--|:--|:--|
| **MQTT** | esp-mqtt (`esp_mqtt_client`) | subscribe `queue_topic`; payload = int or JSON `{"waiting":N}`; parse with cJSON (already a dependency) | real-time; supports TLS + user/pass in `queue_mqtt_uri` |
| **HTTP** | `esp_http_client` | GET `queue_http_url` every `queue_poll_s`; JSON key `queue_json_key` or plain int | simplest; poll interval configurable |
| **PBX / AMI** | raw TCP (`lwip/sockets`) | Asterisk AMI login + `Action: QueueStatus` / `QueueSummary`, parse `Queue:`/`Waiting:` | optional ARI (HTTP+JSON) as a variant later |
| **Local** | internal counter | count inbound calls unanswered within `queue_poll_s`; decrement on answer/end | no external dependency; approximate |

A single **queue task** (prio ~4) owns the active source, reconnects with
backoff, and calls `notification_on_queue()`. Source is hot-swappable from the
Settings page (stop old, start new) — no reboot.

Thresholds: `queue_alert_threshold` (e.g. ≥5 → amber, ≥10 → red) drive the
matrix colour.

---

## 6. Alert acknowledgement (implement all, select in settings)

`alert_ack_mask` bitfield; any enabled method clears the alert.

| Method | Integration |
|:--|:--|
| **Button** | extend `button_task` (`ui_controller.c:710`): if an alert is active, ack it instead of dialling |
| **Touch** | add an `ACK` action on the LVGL alert/incoming screen, wired via `ui_lvgl_set_action_cb` |
| **Web** | `POST /alert_ack` + an alert banner with an ACK button on `/` |
| **Timeout** | `notification_tick()` clears after `alert_timeout_s` (0 = never) |

Re-alert: if unacknowledged, re-trigger every `alert_repeat_s`.
Tone: optional beep via the audio pipeline / buzzer (`alert_tone`).

---

## 7. Settings & NVS schema

### 7.1 `app_settings_t` additions (`components/config_store/config_manager.h`)

| Field | Type | Default | NVS key |
|:--|:--|:--|:--|
| `display_mode` | uint8 | 1 (caller-id) | `disp_mode` |
| `matrix_type` | uint8 | 0 (none) | `mx_type` |
| `matrix_channels` | uint8 | 3 (rgb) | `mx_chan` |
| `matrix_brightness` | uint8 | 60 | `mx_bright` |
| `matrix_width` / `matrix_height` | uint16 | 64 / 32 | `mx_w` / `mx_h` |
| `matrix_layout` | uint8 | 0 (2D) / 1 (single-line) | `mx_lay` |
| `matrix_modules` | uint8 | 8 (MAX7219 chain length) | `mx_mod` |
| `matrix_scan` | uint8 | 0 (auto) | `mx_scan` |
| `queue_source` | uint8 | 0 (off) | `q_src` |
| `queue_alert_threshold` | uint8 | 5 | `q_thr` |
| `queue_poll_s` | uint16 | 10 | `q_poll` |
| `queue_mqtt_uri` | char[128] | "" | `q_mqtt` |
| `queue_topic` | char[64] | "" | `q_topic` |
| `queue_http_url` | char[128] | "" | `q_url` |
| `queue_json_key` | char[32] | "waiting" | `q_key` |
| `queue_pbx_host` | char[64] | "" | `q_host` |
| `queue_pbx_port` | uint16 | 5038 | `q_port` |
| `queue_pbx_user` | char[32] | "" | `q_user` |
| `queue_pbx_secret` | char[64] | "" | `q_secret` |
| `queue_pbx_queue` | char[32] | "" | `q_name` |
| `alert_ack_mask` | uint8 | 0x0B (button+web+timeout) | `a_ack` |
| `alert_timeout_s` | uint8 | 60 | `a_tmo` |
| `alert_repeat_s` | uint8 | 30 | `a_rep` |
| `alert_tone` | uint8 | 0 | `a_tone` |

### 7.2 `hardware_settings_t` additions

```c
// HUB75 (13 used, E optional)
int8_t pin_mx_r1, pin_mx_g1, pin_mx_b1, pin_mx_r2, pin_mx_g2, pin_mx_b2;
int8_t pin_mx_a, pin_mx_b, pin_mx_c, pin_mx_d, pin_mx_e;
int8_t pin_mx_clk, pin_mx_lat, pin_mx_oe;
// MAX7219 8x8 chain
int8_t pin_mx_din, pin_mx_mclk, pin_mx_cs;
```
Persisted in the existing `hw_config` blob. All default to `-1` (= not wired).

### 7.3 `config_manager.c`

Add the new keys to `apply_defaults`, `config_manager_load` and
`config_manager_save` (strings via `get_str_or_keep`, scalars with range
checks, matching the existing style).

---

## 8. Web UI changes (`main/ui_controller.c`)

New **Settings** tabs:

* **Display** — mode (Off / Caller ID / Queue / Alert / Caller+Queue / All),
  matrix type, channels, brightness, panel size, scan, **Test pattern** button.
* **Queue** — source dropdown; conditional fields per source; threshold, poll.
* **Alert** — ack methods (checkboxes), timeout, repeat, tone.

New **Hardware** tab:

* **Matrix** — type (HUB75 / MAX7219 8×8 array / preview), channels, layout
  (2D / single-line), and the pins for the selected backend (HUB75: 14 pins;
  MAX7219: DIN/CLK/CS + module count). Same "blank = keep / -1 = not wired"
  behaviour as the existing tabs.

Index page (`/`):

* Alert banner with an **ACK** button (when an alert is active).
* Queue metric box (reuses the existing gauge style).
* **Matrix preview** — `<pre>` ASCII rendered from `matrix_get_ascii()`.

New endpoints: `POST /alert_ack`, `POST /matrix_test`.

---

## 9. Concurrency & safety (ties to the audit)

* `notification_on_incoming()` runs in `sip_task` → must only set state/signal.
  All drawing happens in the matrix task (audit CON-01/FUN-09 discipline).
* Matrix task priority below audio (10) and SIP (8); never block on I2C/SPI.
* Guard `matrix` + `notification` state with a mutex.
* Avoid the settings race (audit FUN-06): queue/notification read a snapshot,
  not the live `*g_settings` mutated by the HTTP task.
* Authenticate transports (audit SEC-02/03): MQTT user/pass + TLS, HTTP token,
  AMI secret — never cleartext over an untrusted network.

---

## 10. Memory budget (ESP32-S3)

| Item | Approx |
|:--|--:|
| esp-hub75 GDMA buffer (64×32, 8-bit) | 28 KB (single) / 57 KB (double) |
| MAX7219 8×8 array framebuffer (8 modules) | ~64 bytes |
| Logical RGB framebuffer + font | ~7 KB |
| Queue task (MQTT/HTTP/AMI buffers) | ~8–12 KB heap |
| Matrix task stack | 4 KB |
| Existing audio pipeline struct | ~12 KB |
| LVGL draw buffer (TFT) | ~19 KB |

Comfortable on S3 (512 KB SRAM, PSRAM optional). Tight on classic ESP32 — hence
the S3 decision.

---

## 11. Milestones & tasks

### M1 — Matrix bring-up (HUB75 + MAX7219 single-line + font)
- [ ] `matrix_display` component skeleton + C API + font5x7.
- [ ] `matrix_hub75.cpp` (esp-hub75), `idf_component.yml`.
- [ ] `matrix_max7219.c` 8×8 single-line backend (SPI/bit-bang, chain length).
- [ ] S3 pin-map blocks in `app_config.h` (`CONFIG_SIP_MATRIX_HUB75`,
      `CONFIG_SIP_MATRIX_MAX7219`).
- [ ] `hardware_settings_t` matrix pins + Hardware→Matrix tab.
- [ ] Test pattern + brightness; verify 1/16 scan / shift driver on hardware.
- [ ] Wokwi `wokwi-max7219-matrix` part + single-line ticker in the simulation.

### M2 — Caller ID
- [ ] `notification_service` skeleton + wiring in `main.c`.
- [ ] `phonebook_find_by_uri()` + caller→room resolution.
- [ ] Scrolling caller text on the matrix.
- [ ] Web preview (`matrix_get_ascii`) + Display tab.

### M3 — Queue mode (all sources)
- [ ] `queue_state_t` + queue task framework + source hot-swap.
- [ ] MQTT source (esp-mqtt, cJSON).
- [ ] HTTP source (esp_http_client).
- [ ] PBX/AMI source (TCP).
- [ ] Local counting source.
- [ ] Queue rendering + thresholds; Queue tab.

### M4 — Alert mode (all acks)
- [ ] Alert state machine + blink/tone + re-alert.
- [ ] Button ack, touch ack (LVGL), web ack (`/alert_ack`), timeout.
- [ ] Missed-alert log + web banner; Alert tab.

### M5 — Polish / optional backends
- [ ] Mono auto-detect / channel selection.
- [ ] Multi-panel chaining (HUB75 serpentine/zigzag; MAX7219 longer chains).
- [ ] Optional ARI queue variant.
- [ ] Optional: drive HUB75 **and** a MAX7219 line at once (two backends).

---

## 12. Testing strategy

| Layer | How |
|:--|:--|
| Logic (modes, ack, queue parsing) | `matrix_preview` backend + host unit tests for parsers |
| Wokwi | MAX7219 backend (Wokwi emulates MAX7219) + preview over the web UI |
| HUB75 hardware | test pattern → caller ID → queue → alert; check refresh, colour, ghosting |
| Web UI | matrix ASCII preview, `/matrix_test`, `/alert_ack` |
| Queue sources | loopback MQTT broker (mosquitto), local HTTP stub, Asterisk test PBX |

---

## 13. Risks & mitigations

| Risk | Mitigation |
|:--|:--|
| HUB75 DMA interferes with S3 Wi-Fi | level shifter, lower pixel clock, antenna clearance, test early |
| MAX7219 SPI contends with TFT/LVGL SPI2 | use a dedicated SPI3 bus or GPIO bit-bang; slow clock is fine |
| Pin conflicts on the shared S3 | keypad pins repurposed; all pins runtime-configurable |
| SRAM pressure with audio + LVGL | single-buffer first; PSRAM if needed; preview backend on LITE |
| SIP task blocking on matrix draw | state-only callbacks + dedicated matrix task |
| Queue transport security | TLS/user-pass/token; never cleartext (audit SEC-02/03) |
| esp-hub75 C++ in a C project | `extern "C"` shim in `matrix_hub75.cpp` |

## 14. Out of scope (for now)

* LVGL rendering directly onto HUB75 (would allow rich graphics).
* Audio announcements of room numbers.
* Cloud/multi-site queue aggregation.
