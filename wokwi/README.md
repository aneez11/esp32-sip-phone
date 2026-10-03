# Wokwi simulation

Run the firmware in the [Wokwi for VS Code](https://docs.wokwi.com/vscode/getting-started)
extension. This folder holds the helper script and notes; the two files Wokwi
actually reads live in the **repository root**:

| File | Purpose |
|:--|:--|
| `wokwi.toml` | Points Wokwi at the Wokwi build output; optional host port-forward |
| `diagram.json` | ESP32-DevKitC V4 + ILI9341 display + XPT2046 touch |
| `sdkconfig.wokwi.defaults` | Kconfig overlay that enables `CONFIG_SIP_WOKWI_SIM` |
| `wokwi/build.ps1` / `wokwi/build.sh` | Build a separate Wokwi firmware |

> **Simulation is an approximation.** Wokwi emulates the CPU, Wi-Fi, SPI
> displays and touch, but **not I2S audio, a SIP server, or RTP**. See
> [What you can and cannot test](#what-you-can-and-cannot-test) below.

---

## 1. Prerequisites

* **Wokwi for VS Code** extension installed.
* **ESP-IDF** installed and working (`idf.py` on PATH). The project builds with
  ESP-IDF 6.x; see the audit (`docs/AUDIT.md`, BLD-01) about the CI version.
* On Windows, run the build from an **“ESP-IDF PowerShell”** terminal so the
  toolchain environment is loaded.

## 2. Build the Wokwi firmware

Wokwi does **not** compile your code — build first. The firmware gets its own
build directory and its own `sdkconfig`, so your normal `build/` and real
`app_config.h` defaults stay untouched.

```powershell
# Windows (ESP-IDF PowerShell), from the repository root
./wokwi/build.ps1
```

```bash
# Linux / macOS (ESP-IDF environment exported)
./wokwi/build.sh
```

This produces:

```
build_wokwi/flasher_args.json      <- bootloader + partition table + app
build_wokwi/esp32_sip_voice.elf    <- for GDB
```

Useful options: `./wokwi/build.ps1 -Clean`, `./wokwi/build.ps1 -Target esp32s3`
(if you change the board in `diagram.json` to match).

## 3. Run it

1. Open the repository root in VS Code.
2. Press **F1** → **`Wokwi: Start Simulator`**.
3. Watch the **serial monitor** in the Wokwi window:
   * `WIFI: Got IP address: 10.13.37.x` — joined the virtual `Wokwi-GUEST` AP.
   * `SIP_CLIENT: Local IP: 10.13.37.x` then repeated register attempts.
   * `AUDIO: ...` / `CODEC: ...` init logs (audio may be skipped — see below).
4. The **ILI9341** shows the LVGL theme selected by the firmware (default:
   *Voice Assistant*). Press the board's **BOOT** button (GPIO0) to dial/answer
   /hang up, or use the touch overlay if touch registers (see below).

### Open the web UI

`wokwi.toml` forwards `http://localhost:8080` to port 80 on the simulated
device. Once it has an IP, open **<http://localhost:8080>** and sign in with the
factory login `admin` / `esp32sip`.

If `localhost:8080` does not connect, the host port-forward needs the Wokwi IoT
gateway. For local-network access run `wokwigw` and enable the private gateway:
uncomment the `[net] gateway` block in `wokwi.toml`, then restart the simulator.

## 4. Pin map used by the diagram

The diagram matches the project's **classic ESP32** compile-time defaults
(`components/config_store/app_config.h`), so no runtime GPIO configuration is
needed:

| Signal | GPIO | Notes |
|:--|:--:|:--|
| ILI9341 MOSI | 23 | SPI2 (HSPI) |
| ILI9341 SCK | 18 | |
| ILI9341 CS / D/C / RST | 15 / 2 / 4 | |
| ILI9341 LED | 3V3 | backlight |
| XPT2046 MOSI / SCK | 23 / 18 | shares the display bus |
| XPT2046 MISO | 19 | **needed** for touch reads |
| XPT2046 CS / IRQ | 14 / 27 | |
| Call button | 0 | onboard **BOOT** button |

## Viewing logs

ESP-IDF logs go to **UART0 at 115200** and are shown in the Wokwi simulator tab's
**Serial Monitor** automatically. Keep the simulator tab visible: Wokwi pauses
the simulation when the tab is hidden, so output stops until you bring it back.

* Force the monitor open at startup by adding to `diagram.json`:
  `"serialMonitor": { "display": "always", "newline": "lf" }`.
* For a normal VS Code terminal, `wokwi.toml` exposes the serial port over
  RFC2217 on **`rfc2217://localhost:4000`** (115200 baud). Use the VS Code
  *Serial Monitor* extension in TCP mode, PuTTY in Telnet mode, or pyserial.
* Raise/lower verbosity with `idf.py menuconfig` → *Component config → Log
  output*, or at runtime via `esp_log_level_set()`. The current level is
  compiled into `build_wokwi/sdkconfig` (`CONFIG_LOG_DEFAULT_LEVEL`).

Expected log sequence on a good boot:

```
I (xxx) MAIN: Starting ESP32 SIP Client Application
I (xxx) WIFI: WIFI_EVENT_STA_START: Connecting to Wokwi-GUEST...
I (xxx) WIFI: Got IP address: 10.13.37.x
I (xxx) SIP_CLIENT: Local IP: 10.13.37.x
I (xxx) TFT: TFT panel initialized.
I (xxx) UI_LVGL: LVGL ready. Theme 0, 240x320, round=0
I (xxx) SIP_CLIENT: TX REGISTER (CSeq 1)
```

## What you can and cannot test

**Works in the simulator**

* Boot, NVS init, Wi-Fi association to the open `Wokwi-GUEST` network.
* The **web UI** (login, status dashboard, Settings/Hardware/Phonebook tabs)
  through the host port-forward.
* **LVGL rendering** on the ILI9341 and theme selection.
* **Physical call button** (BOOT/GPIO0) → SIP action queueing.
* XPT2046 touch input (needs `TFT_MISO=19`, provided by this build) — the raw
  calibration may need tuning via `TOUCH_*` in `app_config.h`.

**Not simulated / limited**

* **No audio.** Wokwi does not emulate I2S microphones/amps (INMP441/MAX98357A/
  ES8388). The pipeline init either skips or times out harmlessly; there is no
  sound. The call logic and UI still run.
* **No real SIP/RTP server.** The default server `192.168.1.100` is unreachable,
  so the status stays *Registering*. You can point it at a SIP server reachable
  through the Wokwi gateway from the Settings page, but full two-way calling is
  not practical in the simulator.
* **No SRTP/TLS** (not implemented in the firmware anyway).
* `wokwi-ili9341` is the closest emulated panel to the firmware's default
  ST7789; the Wokwi build selects the real ILI9341 driver so the panel works
  correctly.

## How the build was tailored (and why)

Wokwi needs a few things the firmware's production defaults do not provide, so
`CONFIG_SIP_WOKWI_SIM` applies **opt-in** overrides in `app_config.h`:

| Override | Why |
|:--|:--|
| `USE_DISPLAY_ILI9341` | Wokwi emulates ILI9341, not ST7789/GC9A01 |
| `WIFI_SSID "Wokwi-GUEST"`, password `""` | the simulator's virtual AP |
| `WIFI_MIN_AUTHMODE WIFI_AUTH_OPEN` | `Wokwi-GUEST` is open; the production threshold is WPA2 |
| `TFT_MISO 19` | routes the touch controller's MISO so touch works |

Supporting changes (all no-ops unless `CONFIG_SIP_WOKWI_SIM=y`):

* `main/Kconfig.projbuild` — new `SIP_WOKWI_SIM` option.
* `components/config_store/app_config.h` — the overrides and the
  `WIFI_MIN_AUTHMODE` / `TFT_MISO` defaults (defaults unchanged).
* `main/wifi_manager.c` — uses `WIFI_MIN_AUTHMODE` (still WPA2 by default).
* `components/board_hal/display_tft.c` — honours the already-exposed
  `spi_miso` pin, falling back to `TFT_MISO` (still `-1` by default).
* `components/board_hal/idf_component.yml` — adds `espressif/esp_lcd_ili9341`
  so the ILI9341 option links.

To go back to a normal build simply run `idf.py build` as usual — the Wokwi
overrides are only applied when `build_wokwi/sdkconfig` is used.

## Troubleshooting

| Symptom | Fix |
|:--|:--|
| `firmware binary not found` | Build first with `wokwi/build.ps1`; check `build_wokwi/flasher_args.json` exists |
| `The board in diagram.json doesn't match idf.py set-target` | Both must be `esp32` (or change both to `esp32s3`) |
| Display stays black | Confirm the ILI9341 wiring in `diagram.json` and that the build log shows `SIP_PROFILE... ILI9341`; check for `TFT panel initialized` |
| Wi-Fi never connects | Ensure `CONFIG_SIP_WOKWI_SIM=y` was built (serial shows `Connecting to Wokwi-GUEST`) |
| Never leaves *Registering* | Expected: there is no reachable SIP server |
| `localhost:8080` refuses | Port-forward needs the Wokwi IoT gateway (see above) |
