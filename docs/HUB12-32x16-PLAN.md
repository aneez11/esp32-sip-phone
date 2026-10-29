# Plan: HUB12 32x16 display integration

Target board: **ESP32-S3** (16 MB flash), same board that already drives the
MAX7219 8x8x4 ticker on GPIO 10/11/12 and the onboard RGB LED on GPIO 48.
Panel under test: **HUB12, 32 columns x 16 rows, P10 pitch** (10 mm between
LEDs -> 320 x 160 mm). The silkscreen on the panel reads "HUB12". HUB12 is the
12-pin cousin of HUB75: same RGB data lines, only **A and B** address lines
(1/4 scan, 4 rows per scan), no C/D/E. Driver IC unknown yet - likely
ICN2037 / DP5125D / FM6126A (see section 2.1).

Note: the existing matrix_display backend is named MX_HUB75 and uses the
esphome/esp-hub75 driver, which speaks the 16-pin HUB75 protocol. A HUB12
panel is electrically a subset (12 of the 16 pins), so the same driver can
drive it by leaving C/D/E unset - but the scan wiring and address decoding
differ and must be verified on hardware (see section 2.1). If the panel stays
blank/garbled, the fallback is a dedicated HUB12 backend (section 1 option 3).

This plan is deliberately staged so each step is independently buildable and
testable. Nothing here is implemented yet; the existing MX_HUB75 path in
matrix_display is a stub (see section 2).

## 0. Current state (relevant files)

| File | State |
|:--|:--|
| components/matrix_display/backends/matrix_hub75.cpp | Compiles only under CONFIG_SIP_MATRIX_HUB75; hub75_flush() is a no-op that never copies the logical framebuffer; depends on esphome/esp-hub75 which is not in managed_components and does not build under IDF 6.1 (.intr_priority removed from gdma_channel_alloc_config_t). |
| components/matrix_display/idf_component.yml | esphome/esp-hub75 dependency commented out for exactly that reason. |
| components/config_store/app_config.h | S3 default HUB75 pin map exists (GPIO 10-21, 42) but reuses the MAX7219 pins (10-12) - a conflict that must be resolved before both panels can coexist. |
| components/config_store/config_manager.{h,c} | matrix_type/width/height/channels/brightness/layout/modules + HUB75 pins already in app_settings_t/hardware_settings_t and NVS. |
| main/ui_controller.c | Hardware page has no Matrix tab; Settings has no Display tab. Matrix config is invisible to the user. |
| main/matrix_status.c | Renders state on whatever matrix handle exists; layout assumes 32x8 (MAX7219). HUB75 32x16 needs a 2D layout path. |
| main/main.c | build_matrix_cfg() already handles MX_HUB75 (width/height/pins). |

Conclusion: the data model and config plumbing exist; what is missing is a
working driver, a real flush, and UI to configure it.

## 1. Driver decision (blocker)

esphome/esp-hub75 is unavailable on IDF 6.1. Three options, in order of
preference:

1. Patch esphome/esp-hub75 to IDF 6.1 - remove the .intr_priority field,
   re-add as a managed dependency, build. Lowest risk; reuses tested DMA code.
   Estimated 0.5 day.
2. Port mrcodetastic/ESP32-HUB75-MatrixPanel-DMA into an IDF component (it is
   Arduino-oriented). More work, well-documented timing.
3. Write a minimal HUB12 driver using esp_driver_rmt or bit-bang for a
   32x16 / 1/4-scan (A,B) panel. ~2-3 days, but gives full control and avoids
   third-party breakage. Given the project already bit-bangs MAX7219, this is
   feasible and removes the external dependency entirely.

Recommendation: start with option 1 (patch); fall back to option 3 if the patch
reveals further API drift. Do not attempt option 2 unless 1 and 3 fail.

## 2. Fix the HUB12/HUB75 backend (matrix_hub75.cpp)

- hub75_init: keep as-is but make scan_wiring selectable (1/4 scan for a
  16-row HUB12 panel: address lines A,B only, 4 rows per scan).
  esphome/esp-hub75 exposes Hub75ScanWiring::STANDARD_TWO_SCAN (1/16) and
  ..._FOUR_SCAN (1/4); use FOUR_SCAN for this panel. Add a config field
  matrix_scan (already in the plan's NVS schema, not yet in matrix_cfg_t - add
  uint8_t scan).
- hub75_flush: must copy the core's RGB888 framebuffer into the driver's
  framebuffer. The backend currently has no access to d->fb (the vtable passes
  only ctx). Two fixes:
  - store fb and fb_len in hub75_ctx_t (init already receives them),
  - in flush(), convert RGB888 -> RGB888_32 and call the driver's bulk draw API
    (or set_pixel loop for 32x16 = 512 pixels, trivial at 10 Hz).
- hub75_deinit: free argb (already done) and the driver.
- Gate the whole file on CONFIG_SIP_MATRIX_HUB75 (already).

## 3. Config / data model changes

- Add uint8_t matrix_scan to matrix_cfg_t and app_settings_t (NVS key mx_scan),
  default 0 = auto (1/4 for height<=16, 1/16 otherwise).
- Add uint8_t matrix_pixel_aspect to matrix_cfg_t and app_settings_t (NVS key
  mx_par): 0 = square (1:1), 1 = wide (2:1, pixels twice as wide as tall),
  2 = tall (1:2). Only meaningful for HUB panels; MAX7219 is always square.
  Stored as the integer ratio (par_x / par_y) scaled by 100 so future panels
  with arbitrary ratios (e.g. 4:3, 3:2) are representable without a new enum.
- Add bool chain_reversed already added for MAX7219; generalise to a
  per-backend flag if needed.
- build_matrix_cfg() in main.c already maps HUB75 pins from hardware_settings_t;
  verify the S3 default pin map does not collide with the MAX7219 ticker
  (currently GPIO 10-12 are shared). Decision needed: either (a) use the S3
  HUB75 map from section 1.4 of FEATURE-PLAN.md (GPIO 10-21, 42, 1, 2) and
  disable the MAX7219 ticker, or (b) move the MAX7219 pins to a free set (e.g.
  35-37) so both can coexist. Recommend (a) for the HUB75 build, (b) only if
  dual-panel is required (FEATURE-PLAN section 5 "optional").

## 4. Web UI (configurable display)

A single **Display** tab on the Settings page and a **Matrix** tab on the
Hardware page, both driven by the same `matrix_type` selector. Switching type
re-renders the pin form for that backend only (HUB12: 12 pins, HUB75: 14 pins,
MAX7219: 3 pins + module count, Preview: none). Blank fields keep the stored
value; -1 means "not wired".

### 4.1 Settings -> Display tab

- **Display mode**: Off / Caller ID / Queue / Alert / All (drives `display_mode`).
- **Display type**: None / MAX7219 8x8 chain / HUB12 (32x16) / HUB75 (64x32) /
  Preview (ASCII only). Changing type restarts the device (different driver).
- **Colour**: Mono R / Mono G / Mono B / RGB (HUB panels only; MAX7219 ignores
  it - its LEDs are fixed colour).
- **Layout**: 2D (full panel) / Single line (8-px ticker). Auto-suggested per
  type (MAX7219 -> single line, HUB -> 2D) but overridable.
- **Multi-line layout** (2D panels only): configure up to N independent line
  slots, each with its own data source, scale, alignment and scroll behaviour
  (see section 4.5). Defaults are sensible per state (Ready -> "READY"/extension,
  Incoming -> "RING" + caller, Active -> "TALK" + caller).
- **Brightness**: 0-100 % slider. Applied **live** via `matrix_set_brightness()`
  (no restart) and persisted. The slider's max effective value is clamped per
  backend (MAX7219 intensity register is 0-15, so 100% maps to 15; HUB panels
  use the driver's 0-100 range directly).
- **Panel geometry**: width / height (HUB only; MAX7219 derives from module
  count). Module count for MAX7219 chains (1-8).
- **Pixel aspect ratio**: Square (1:1) / Wide (2:1) / Tall (1:2) / Custom
  (enter horizontal and vertical pitch in mm; the ratio is derived). Only
  applies to HUB panels; MAX7219 is always square. Changing it re-renders text
  and animations without a restart.
- **Scan**: Auto / 1/4 / 1/8 / 1/16 (HUB only; Auto picks by height).
- **Test pattern** button -> `POST /matrix_test` draws a known pattern
  (checkerboard + border + "TEST") and reports the result; useful to verify
  wiring before configuring content.
- **Screensaver**: enable/disable + idle timeout (s) before the panel switches
  from status text to the saver animation. Plus a **screensaver picker** so the
  user is not stuck with the fixed rotation:
  - **Mode**: Off / Single (one chosen effect) / Rotate (cycle through enabled
    effects) / Random.
  - **Enabled effects** (checkboxes): Bouncing pixel, Diagonal rain, Snake,
    Sparkle, Scrolling text, Level meter, Clock (if NTP synced).
  - **Texts**: up to 4 custom strings (32 chars each) shown by the "Scrolling
    text" saver, e.g. company name, room label, phone extension. Empty slots
    fall back to "SIP PHONE" / the configured display name.
  - **Speed**: Slow / Normal / Fast (maps to px/s and frame delay).
  - **Brightness in saver**: separate 0-100 % so the panel can dim at night
    (applied live).
  - Stored in NVS: `ss_mode`, `ss_mask`, `ss_text[0..3]`, `ss_speed`,
    `ss_idle_s`, `ss_bright`. Hot-swappable from the web UI, no reboot.

### 4.2 Hardware -> Matrix tab

Backend-specific pin forms, all optional (-1 = not wired, blank = keep):

- **MAX7219 (3 pins)**: DIN, CLK, CS. Plus module count (shared with Settings).
- **HUB12 (12 pins)**: R1, G1, B1, R2, G2, B2, A, B, CLK, LAT, OE, (GND).
- **HUB75 (14 pins)**: R1, G1, B1, R2, G2, B2, A, B, C, D, E, CLK, LAT, OE.
- **Preview**: no pins.

A small **pin conflict check** runs server-side: if a pin is claimed by two
backends or by I2S/I2C/SPI, the save is rejected with a message naming the
conflict (reuses the existing `parse_pin` validation).

### 4.4 Screensaver config schema (NVS)

| Field | Type | Default | NVS key |
|:--|:--|:--|:--|
| `ss_enabled` | bool | 1 | `ss_en` |
| `ss_mode` | uint8 | 1 (rotate) | `ss_mode` |
| `ss_mask` | uint8 (bitmask of enabled effects) | 0x1F (first five) | `ss_mask` |
| `ss_text[0..3]` | char[32] | "SIP PHONE", "", "", "" | `ss_t0`..`ss_t3` |
| `ss_speed` | uint8 (0=slow,1=normal,2=fast) | 1 | `ss_spd` |
| `ss_idle_s` | uint8 (0 = disabled) | 30 | `ss_idle` |
| `ss_bright` | uint8 (0-100, 0 = follow main brightness) | 0 | `ss_br` |

The bitmask bits: 0=bounce, 1=rain, 2=snake, 3=sparkle, 4=scroll text,
5=level meter, 6=clock. Unknown bits are ignored. With `ss_mode=Single` the
first enabled bit is used; `Rotate`/`Random` walk the enabled set.

### 4.5 Multi-line layout config (NVS)

Stored per slot (max 4 slots; unused slots have source = Blank):

| Field | Type | Default | NVS key |
|:--|:--|:--|:--|
| `line_count` | uint8 (1-4, capped by panel height) | 1 | `ln_cnt` |
| `line_src[n]` | uint8 (data source enum) | per-state default | `ln_s0`..`ln_s3` |
| `line_scale[n]` | uint8 (0 = auto, 1-4) | 0 | `ln_sc0`..`ln_sc3` |
| `line_span[n]` | uint8 (1-4 rows; 2 = double height) | 1 | `ln_sp0`..`ln_sp3` |
| `line_align[n]` | uint8 (0=left,1=centre,2=right) | 0 | `ln_al0`..`ln_al3` |
| `line_scroll[n]` | bool | 1 (left-aligned) / 0 (others) | `ln_sr0`..`ln_sr3` |

Data source enum: 0=Blank, 1=Caller, 2=Queue count, 3=Alert text, 4=SIP user,
5=SIP server, 6=IP address, 7=Uptime, 8=Volume, 9-12=Custom text 1-4,
13=Time, 14=Date, 15=Status (READY/RING/TALK/...).

The web UI exposes this as a small table: one row per slot with dropdowns for
source/scale/span/align and a scroll checkbox. Span defaults to 1; setting it
to 2 (or more) greys out the following slot(s) since they are consumed.
Slots beyond the panel's capacity are hidden (not just disabled). Changes apply
live, no reboot.

### 4.3 Index page (`/`)

- Render `matrix_get_ascii()` in a `<pre>` block so the panel can be previewed
  without hardware; refreshes with the page.
- Queue metric box and alert banner with an **ACK** button when an alert is
  active (per FEATURE-PLAN section 6/8).

## 5. Status renderer (matrix_status.c)

- Detect layout: if MX_LAYOUT_LINE, keep the current 8-px ticker behaviour.
- If MX_LAYOUT_2D (HUB75), render 2D content: caller ID large (scale 2),
  queue count centred, alert flashing full-screen. Use matrix_draw_text_center
  with y offsets.
- Screen savers for HUB75: reuse the existing ones but scale to 32x16; add a
  2D variant (bouncing ball, rain columns) for the larger panel.

## 5b. Screensaver handling (state machine)

The screensaver is **not** a separate task; it is a mode of the existing
`matrix_status` task, gated by idle time and the `ss_*` settings. This keeps
drawing single-threaded (no mutex contention with the status renderer).

### 5c.1 When it activates

1. The task tracks `idle_since` (last time the panel state or any setting
   changed). On every tick, if `ss_enabled` and `ss_idle_s > 0` and
   `now - idle_since >= ss_idle_s`, the saver phase begins.
2. On activation the task:
   - snapshots the current `ss_mode`, `ss_mask`, `ss_text[]`, `ss_speed`,
     `ss_bright` into local variables (so a mid-saver settings edit is not
     torn),
   - clears the framebuffer and resets all animation state,
   - if `ss_bright > 0`, calls `matrix_set_brightness(ss_bright)`; otherwise
     keeps the main brightness.
3. Activation is **suppressed** while a call is in progress (`MS_INCOMING`,
   `MS_CALLING`, `MS_ACTIVE`) and while an alert is unacknowledged, even if
   the idle timer has elapsed. The saver only runs in `MS_READY` (and
   `MS_SETUP`/`MS_OFFLINE` if the user opts in via a `ss_run_in_setup` flag).

### 5c.2 Effect selection

- **Single**: always the first enabled bit in `ss_mask`.
- **Rotate**: advance to the next enabled bit every `ss_mode_hold_s` seconds
  (default 6). Wraps around the enabled set.
- **Random**: pick a random enabled bit on each transition, never the same
  twice in a row.
- **Off**: saver disabled entirely; the panel shows the normal status text.

Enabled effects (bitmask in `ss_mask`):

| Bit | Effect | 8-px ticker | 2D panel (32x16+) |
|:--:|:--|:--|:--|
| 0 | Bounce | single pixel bouncing | bouncing ball (2x2) |
| 1 | Rain | diagonal falling dots | rain columns |
| 2 | Snake | bottom-row crawl | full-height snake |
| 3 | Sparkle | random twinkling pixels | sparkle field |
| 4 | Scroll text | marquee of `ss_text[n]` | marquee on middle row |
| 5 | Level meter | bottom-row bars | full-height bars |
| 6 | Clock | (disabled on 8-px; needs 2 lines) | time on row 0, date on row 1 |

If the mask is 0 (no effects enabled), the saver falls back to "Scroll text"
with `ss_text[0]` so the panel is never blank.

### 5c.3 Text handling

- `ss_text[0..3]` are rotated by the "Scroll text" effect: each pass uses the
  next non-empty string, wrapping around. Empty slots are skipped (not shown
  as blank frames).
- If all four slots are empty, the effect uses the SIP display name, then
  "SIP PHONE" as the final fallback.
- Text is drawn with the same fit rules as status text (section 5b): centre if
  it fits, scroll if it overflows, scale chosen by panel height.

### 5c.4 Exit conditions

The saver exits immediately (and restores main brightness) when any of:
- panel state changes (incoming call, Wi-Fi drop, SIP re-registration, ...),
- a volume change is detected (the `VOL n%` overlay takes priority),
- `ss_enabled` is turned off from the web UI,
- the idle timer is reset by a web request or button press.

On exit the task restores the main brightness, clears the framebuffer, and
resumes the normal status render. No reboot required.

### 5c.5 Concurrency

- The saver runs inside `matrix_status_task` (same task that renders status),
  so there is no second writer to the framebuffer and no extra mutex. Settings
  are read once at activation (snapshot), so a mid-saver edit to `ss_text`
  takes effect on the next activation, not mid-animation.
- `matrix_set_brightness()` is the only call that touches the backend during
  the saver; it is mutex-protected by the component.

## 5c. Content fitting across panel sizes

The same status content ("READY", "CALL 2001", "VOL 80%") must look right on
an 8x8 module, a 32x8 ticker, a 32x16 HUB and a 64x32 HUB. The renderer
picks scale and layout from the panel geometry, not from a hard-coded size:

| Panel | Layout | Text scale | Strategy |
|:--|:--|:--:|:--|
| 8x8 (1 module) | single | 1 (5x7, clipped to 8x8) | scroll if longer than 8 px |
| 32x8 ticker | single line | 1 | centre short text, scroll long text |
| 32x16 HUB | 2D | 2 (10x14 glyphs, clipped to 16 px) | centre vertically + horizontally; scroll if wider than 32 px |
| 64x32 HUB | 2D | 2 or 3 | centre; queue count at scale 3 (15x21) |

Rules applied by `matrix_status`:
- **Scale selection**: start at scale 2 for height >= 16, scale 1 otherwise;
  drop to scale 1 if the text would overflow the panel width at the chosen
  scale; never exceed scale 4.
- **Horizontal fit**: if `text_width(text, scale) <= panel_width`, centre it;
  otherwise scroll at `px_per_s` from the `ss_speed` setting (scaled by panel
  width: a 64-wide panel scrolls faster in px/s than a 32-wide one for the
  same perceived speed).
- **Vertical fit**: centre on `(height - glyph_height) / 2`; for single-line
  layouts (height == 8) y is forced to 0.
- **Long text** (caller URIs, server names): always scrolled, never clipped,
  even on wide panels.

### Multi-line layout (independent data per line)

Auto-wrapping one message across rows is not useful on a status display: the
user wants **each row to carry different data** (e.g. row 0 = caller, row 1 =
queue count, row 2 = alert). So the panel is treated as a fixed set of
**line slots**, and each slot is independently assigned a data source, scale
and alignment. The number of slots is derived from the panel height:

| Panel | Slots at scale 1 | Slots at scale 2 |
|:--|:--:|:--:|
| 8x8 (1 module) | 1 | 0 (scale 1 only) |
| 32x8 ticker | 1 | 0 (scale 1 only) |
| 32x16 HUB | 2 | 1 |
| 64x32 HUB | 4 | 2 |

- **Slot count** = `floor(panel_height / (FONT5X7_H * scale + scale))`, capped
  by the number of configured slots (max 4). Unused slots are blank, not
  skipped, so rows stay aligned.
- **Row span**: a slot can occupy 1 or 2 (or more, up to the panel height)
  row-bands via `line_span[n]`. A span-2 slot draws its content at double
  height (scale 2 font on a 32x16 panel, or scale 1 stretched) and consumes
  the next slot, which is then unavailable for its own content. Example: slot 0
  span 2 on a 32x16 panel shows a big "CALL" across both rows; slot 1 is
  auto-skipped. This is how a single headline gets emphasis without wasting
  the panel.
- **Each slot** has its own:
  - **Data source** (one of: Caller, Queue count, Alert text, SIP user,
    SIP server, IP address, Uptime, Volume, Custom text 1..4, Time, Date,
    Status, Blank).
  - **Scale** (Auto / 1..4). Auto picks the largest scale that fits the slot
    height.
  - **Alignment** (Left / Centre / Right). Left-aligned long text scrolls;
    centred/right-aligned text is clipped at the panel edge.
  - **Scroll** (on/off): when on, over-wide text scrolls horizontally within
    that slot's row band; when off, it is clipped.
- **Vertical alignment**: each slot occupies a fixed band of `slot_height` px;
  text is vertically centred inside its band. A single-line slot on a tall
  panel therefore sits in the middle of its band, not hugging the top.
- **Mixed scales**: slot 0 can be scale 2 ("CALL") while slot 1 is scale 1
  (caller URI scrolling), because scale is per-slot.
- **Defaults**: a fresh device gets sensible per-state defaults (Ready -> slot0
  "READY"/extension; Incoming -> slot0 "RING", slot1 caller; Active -> slot0
  "TALK", slot1 caller). The user can override any slot in the web UI.
- **Screensavers on tall panels**: the 2D savers (bounce, rain, snake) use the
  full height and ignore slot layout; text savers render on slot 0 only.
  Single-line tickers keep their 8-px behaviour.
- **Custom text slots** pull from the screensaver text list (section 4.4) so
  the same strings can be shown as status lines or as saver content.
- **Numbers** (queue count, volume): drawn with `matrix_draw_number` at the
  largest scale that fits in both dimensions, centred.
- **Aspect ratio** (section 3/5) is applied after fit selection, so a 2:1
  panel uses the same scale logic but stretches glyphs horizontally.
- **Screensavers** scale with the panel: bounce uses the full width/height,
  rain fills all columns, snake spans the width, sparkle density scales with
  pixel count. A saver that does not fit (e.g. a 10-char scroll on an 8-wide
  panel) is skipped rather than clipped.

`matrix_text_width()` and `matrix_width()/height()` (added in the MAX7219
work) are the single source of truth for fit decisions; no hard-coded panel
sizes anywhere in the renderer.
- **Pixel aspect ratio**: the renderer keeps a logical square-pixel coordinate
  system and scales horizontally by `matrix_pixel_aspect` when drawing. Text
  glyphs are stretched to match (a 5x7 font becomes 5*par x 7 logical pixels),
  and the bounce/rain/snake animations move in logical units then map to
  physical pixels. This keeps "READY" readable on a 2:1 panel instead of
  looking squashed. The web ASCII preview applies the same scaling so what the
  user sees in the browser matches the panel.
- Screensaver selection driven by the new `ss_*` settings (section 4.4): effect
  set, custom texts, speed, idle timeout and dim brightness. Hot-swappable from
  the web UI without reboot.

## 6. Validation / test plan

1. Hardware: wire the 32x16 panel per the section 1.4 pin map, 5 V supply with
   bulk cap, level shifter, common ground. Confirm no Wi-Fi drop (DMA vs Wi-Fi
   interference is the #1 risk).
2. Driver: test pattern -> solid colours -> gradient. Verify scan wiring
   (1/4 vs 1/8) - a wrong scan shows a doubled/half image.
3. Software: build with CONFIG_SIP_MATRIX_HUB75=y, confirm matrix_status
   shows "AP SETUP ..." on the panel, then "READY", then a simulated
   incoming call renders "RING"/"CALL <caller>".
4. Wokwi: keep the MAX7219 backend for logic testing (Wokwi emulates
   MAX7219, not HUB75).
5. Memory: check heap after init (HUB75 32x16 ~ 7 KB single buffer - much
   smaller than 64x32).

## 7. Risks

| Risk | Mitigation |
|:--|:--|
| esphome/esp-hub75 still broken on 6.1 | Fall back to option 3 (minimal driver) early; do not block M2/M3 on it. |
| DMA vs Wi-Fi interference | Test Wi-Fi throughput with the panel running; lower pixel clock if needed. |
| Pin collision with MAX7219 ticker | Decide (a) vs (b) in section 3 before wiring. |
| 1/4 vs 1/8 scan mismatch | Expose matrix_scan in the Hardware tab; default to auto by height. |
| Non-square pixels distort text | matrix_pixel_aspect scales glyph width and animation step; web ASCII preview applies the same scaling so the browser matches the panel. |
| Flash size (already 16 MB) | No change needed. |

## 8. Milestones

- M0 (1 day): driver decision + pin map decision + HUB75 backend flush fix
  + scan wiring. Deliverable: a solid-colour test pattern on the real panel.
- M1 (1 day): web Hardware/Settings tabs for matrix + /matrix_test endpoint
  + ASCII preview on /.
- M2 (0.5 day): 2D status renderer (ready/incoming/active) + multi-line
  layout (per-slot data source/scale/align/scroll) + screensavers.
- M2b (0.5 day): screensaver picker (mode, enabled-effects checkboxes,
  custom texts, speed, idle timeout, dim brightness) + NVS wiring.
- M3 (0.5 day): calibration (brightness, scan, colour channel) + docs.

Total estimate: ~3 days, assuming option 1 (patch) succeeds. If option 3
(minimal driver) is needed, add ~2 days.

## 9. Out of scope

- HUB75 + MAX7219 simultaneously (FEATURE-PLAN section 5 "optional").
- LVGL rendering onto HUB75.
- Multi-panel chaining / serpentine.
EOF
echo written