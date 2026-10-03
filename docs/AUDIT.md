# ESP32-SIP-Voice — Full System Audit

| | |
|---|---|
| **Project** | ESP32-SIP-Voice (`esp32_sip_voice`) |
| **Audited revision** | `8a8900b` (branch `main`) |
| **Firmware reported** | v2.4.0 |
| **Audit date** | 2026-10-03 |
| **Auditor** | OpenCode automated review |
| **Scope** | Firmware (`main/`, `components/`), build/config, CI, simulator, documentation |
| **Method** | Full source read + static review of control/data flow, security, concurrency and protocol correctness. No live hardware or network testing was performed. |

> This is a *static* audit. Findings that depend on timing, network loss or specific
> hardware were derived by code inspection and are marked accordingly. No code was
> changed as part of the audit.

---

## 1. Executive summary

The project is an impressively complete hobby-to-production SIP/VoIP phone for
ESP32-class hardware. It contains a real SIP stack (REGISTER/INVITE/ACK/BYE,
digest auth, SDP), a real RTP path with jitter buffer and PLC, real G.711/G.722
codecs, a software NLMS AEC, LVGL UI themes, a web configuration portal and a PC
simulator. The code is generally well structured, defensive about buffers, and
documents its trade-offs.

However, the audit found **one critical** and **several high-severity** issues
that materially affect security, reliability and stated functionality. In
priority order they are:

1. **Inbound SIP is completely unauthenticated** while *Speaker* mode
   auto-answers — anyone who can reach the device on the LAN (or spoof a UDP
   source) can open the microphone and send DTMF.
2. **Hostname DNS resolution is broken** (`dns_gethostbyname` called with a
   `NULL` callback, which returns `ERR_INPROGRESS` for any uncached name), so
   domain-based SIP servers and the STUN hostname silently fail.
3. **Outgoing calls cannot be cancelled** on the wire (no `CANCEL` is ever
   sent) and a **retransmitted INVITE is rejected with `486 Busy Here`**,
   breaking normal lossy-network operation.
4. **The web UI is plaintext HTTP and secrets live in unencrypted NVS**; flash
   encryption / secure boot / NVS encryption are all disabled.
5. **CI is inconsistent with the source** (pinned IDF v5.1.4 while the build
   requires `esp_driver_i2s`, introduced in IDF ≥ 5.4; the checked-in build used
   IDF 6.1.0).

A number of medium findings concern a **data race on the shared
`app_settings_t`**, **non-atomic cross-task state**, an **audio-pipeline
use-after-free window on stop**, and the **web Hardware page not applying the
touch GPIOs it exposes**.

The detailed findings, evidence and remediation guidance are below.

---

## 2. System overview (as audited)

### 2.1 Layers

```
app_main (main.c)
  ├─ config_store      NVS settings (sip_phone / hw_config) + app_config.h defaults
  ├─ storage           NVS phonebook (namespace "phonebook")
  ├─ board_hal         TFT (ST7789/ILI9341/GC9A01), XPT2046 touch, matrix keypad
  ├─ ui_lvgl           LVGL themes + touch indev + display glue
  ├─ wifi_manager      STA + AP captive portal (open AP + DNS hijack)
  ├─ audio_pipeline    I2S task, codec dispatch, AEC, wake word, volume
  │    ├─ g711_codec.c        µ-law / A-law (in main/)
  │    └─ audio_pipeline/*    G.722, OPUS wrapper, NLMS AEC, WakeNet
  ├─ sip_client        UDP SIP UA + STUN + NAT keep-alive + SDP
  ├─ rtp_handler       RTP socket, jitter buffer, PLC
  ├─ ui_controller     esp_http_server web UI + physical button task
  └─ display.c         legacy stub (no-op)
```

`main/` owns signalling, RTP and the audio pipeline; `components/` holds
reusable drivers/DSP/UI and never depends on `main`. `config_store` is the
shared seam (good design).

### 2.2 FreeRTOS tasks

| Task | Prio | Stack | Created in | Notes |
|:--|:--:|--:|:--|:--|
| `main` | 1 | 8192 | IDF | init only |
| `app_ctrl` | 6 | 4096 | `main.c:204` | state poll every 200 ms |
| `sip_task` | 8 | 10240 | `sip_client.c:330` | owns SIP socket; `select()` 200 ms |
| `audio_io` | 10 | 6144 | `audio_pipeline.c:225` | 20 ms frames; highest prio |
| `button_task` | 5 | 2048 | `ui_controller.c:1833` | polls `BUTTON_GPIO` |
| `dns_task` | 5 | 2048 | `wifi_manager.c:99` | AP mode only |
| `lvgl` | 4 | 6144 | `ui_lvgl.c:426` | `lv_timer_handler` every 10 ms |
| esp_http_server | 5 | 6144 | `ui_controller.c:1786` | single task, all handlers |
| FreeRTOS timer | 1 | 2048 (IDF default) | `registration_timer` | queues REGISTER only |
| lwip tcpip | 18 | 3072 | IDF | network core |

### 2.3 NVS layout

| Namespace | Keys | Written by |
|:--|:--|:--|
| `sip_phone` | wifi_ssid/pass, sip_server/user/pass/domain/name/target/port, web_user/pass, audio_out, volume, role, auto_answer | `config_manager.c` |
| `hw_config` | `hw_settings` blob (GPIO pins + theme) | `config_manager.c` |
| `phonebook` | `name_N` / `uri_N` (N = 0..9) | `phonebook.c` |

### 2.4 Build facts

* Target in checked-in `sdkconfig`: **ESP32 (classic)**, ESP-IDF **6.1.0**, 4 MB
  flash, no PSRAM, no wake word.
* `sdkconfig.defaults`: 4 MB, `FREERTOS_HZ=1000`, Montserrat fonts, WakeNet
  model in SPIFFS.
* `partitions.csv`: `nvs` 0x6000, `phy_init`, single `factory` app — **no `model`
  partition**.
* `managed_components`: lvgl 8.4.0, esp-sr 2.5.3, esp_lcd_gc9a01 2.0.4.
* CI (`.github/workflows/build.yml`): IDF **v5.1.4**, targets `esp32` and
  `esp32s3`, plus SDL simulator.

---

## 3. Findings summary

| ID | Sev | Area | Finding |
|:--|:--:|:--|:--|
| SEC-01 | **Critical** | SIP | Inbound requests unauthenticated; Speaker mode auto-answers → open mic + DTMF |
| SEC-02 | High | Web | Plaintext HTTP; credentials & sessions in the clear |
| SEC-03 | High | Storage | Secrets in unencrypted NVS; no flash/NVS encryption or secure boot |
| FUN-01 | High | SIP | Hostname resolution broken (`dns_gethostbyname` with NULL callback) |
| FUN-02 | High | SIP | Outgoing INVITE cannot be cancelled; Timer B abandons transaction |
| FUN-03 | High | SIP | Retransmitted INVITE during ringing answered `486 Busy Here` |
| BLD-01 | High | Build | CI IDF v5.1.4 vs `REQUIRES esp_driver_i2s` (IDF ≥ 5.4) |
| CON-01 | High | Concurrency | Audio-pipeline stop can free RTP session under a blocked audio task |
| SEC-04 | Medium | Web | No CSRF token; state-changing `GET /logout` |
| SEC-05 | Medium | Web/Wi-Fi | Factory creds + open setup AP (partly documented) |
| SEC-06 | Medium | RTP | RTP packets/SSRC/source not validated |
| FUN-04 | Medium | SIP | REGISTER has no retransmission/timeout handling |
| FUN-05 | Medium | SIP | REGISTER `From` tag reuses the active-call tag |
| FUN-06 | Medium | Concurrency | Unlocked `*g_settings = updated` racing the SIP task |
| FUN-07 | Medium | Boot | AP-fallback race: SIP/audio init before async AP mode settles |
| FUN-08 | Medium | Drivers | `codec_set_volume` I2C access not serialized |
| FUN-09 | Medium | Audio | `audio_pipeline_start()` failure ignored → ACTIVE call with no audio |
| FUN-10 | Medium | Web/HW | Hardware-page touch pins are ignored by `touch_driver.c` |
| BLD-02 | Medium | Docs | README claims IDF v4.4+; code needs IDF 5.x I2S API |
| BLD-03 | Medium | Build | `CONFIG_MODEL_IN_SPIFFS=y` + esp-sr but no `model` partition |
| SEC-07 | Low | SIP | Digest `nc` hard-coded to `00000001` |
| SEC-08 | Low | Web | Global login lockout (DoS), non-constant-time compares |
| SEC-09 | Low | SIP | SIP responses accepted without branch/source validation |
| FUN-11 | Low | Keypad | Matrix keypad is dead code; never read for dialing |
| FUN-12 | Low | Audio | Odd timeout arithmetic in `i2s_channel_read` calls |
| FUN-13 | Low | RTP | RTP port not STUN-mapped / no symmetric RTP (NAT) |
| FUN-14 | Low | Display | `ESP_ERROR_CHECK` in `display_tft_init` aborts on wiring error |
| FUN-15 | Low | Codec | OPUS advertised by SDP only if libopus linked; else silent zero frames |
| FUN-16 | Low | SIP | Header search not line-anchored (`ci_strstr`) |
| CON-02 | Low | RTP | Jitter-buffer counters can desync; drain loop unbounded-ish |
| BLD-04 | Low | Hygiene | Duplicate source tree under `.kilo/worktrees/` |
| BLD-05 | Low | Docs | `APP_VERSION` v2.4.0 vs `User-Agent: .../2.3` |
| DOC-01 | Low | Docs | README overstates CANCEL, keypad dialing, esp-sr AEC |

---

## 4. Detailed findings

### SEC-01 — Inbound SIP is unauthenticated (Critical)

**Where:** `main/sip_client.c:1442-1585` (request branch of
`process_incoming_sip`), auto-answer at `main/sip_client.c:1493-1501` and
`681-696`.

**Description:** The UA never challenges inbound requests. `INVITE`, `BYE`,
`CANCEL` and `INFO` are processed on CSeq/Call-ID match alone; the source IP is
never checked and no `401/407` is issued. Any host that can deliver a UDP
datagram to port 5060 can:

* place a call to the device (it rings, or **auto-answers in Speaker mode**,
  opening the microphone to the attacker);
* send `INFO` with `Signal: …` (DTMF) which is only logged but still accepted
  (`sip_client.c:1560-1576`);
* tear down an active call with a forged `BYE`.

On an open Wi-Fi/captive network, or behind a NAT that permits 5060, this is
directly exploitable. For a doorbell/paging/intercom device this is the most
serious issue found.

**Remediation:**
* Challenge inbound `INVITE` (and `BYE`/`INFO` if desired) with `401`/`407`
  digest auth and validate the response before acting; reuse the existing
  digest machinery used for outbound registration.
* At minimum, restrict accepted source addresses to the configured SIP
  server/proxy and validate the `Via`/`Contact`/`From` domain.
* Do not auto-answer (Speaker mode) unless the call was authenticated, or gate
  it behind an allow-list.

---

### SEC-02 — Plaintext web UI (High)

**Where:** `main/ui_controller.c:1786-1825` (`HTTPD_DEFAULT_CONFIG`, no TLS);
session/login at `40-151`, `787-841`.

**Description:** `esp_http_server` is started without TLS. Login credentials,
the `ESPAUTH` session cookie and every setting (including SIP/web passwords)
cross the network in cleartext. The cookie is `HttpOnly; SameSite=Lax` but not
`Secure` (and cannot be, over HTTP). Anyone on the LAN path can sniff or
session-hijack.

**Remediation:** Serve the UI over HTTPS (`httpd_ssl_config_t` + certificate),
redirect HTTP→HTTPS, and set `Secure` on the cookie. If TLS is too heavy, at
minimum document that management must occur on a trusted segment.

---

### SEC-03 — Secrets at rest are unencrypted (High)

**Where:** `components/config_store/config_manager.c:100-125`;
`components/storage/phonebook.c`; `sdkconfig`.

**Description:** Wi-Fi, SIP and web passwords are written with
`nvs_set_str()` in plaintext. `CONFIG_NVS_ENCRYPTION`, `CONFIG_FLASH_ENCRYPTION_ENABLED`
and `CONFIG_SECURE_BOOT` are all unset. Anyone with physical access can dump
flash (`esptool.py read_flash` / `espefuse`) and recover all credentials.

**Remediation:** Enable flash encryption (and optionally NVS encryption +
secure boot) for production images, or derive an NVS encryption key from a
provisioned secret. Document a factory-provisioning flow.

---

### SEC-04 — Missing CSRF protection (Medium)

**Where:** all POST handlers `main/ui_controller.c:1251, 1439, 1456, 1539,
1559, 1693`; `logout_get_handler` at `843`.

**Description:** No anti-CSRF token or `Origin`/`Referer` check exists.
`SameSite=Lax` prevents cookies from riding cross-site POSTs, which mitigates
the common case, but `GET /logout` is state-changing and *is* sent on top-level
cross-site navigation, so a third-party page can sign the user out. If the
cookie policy were ever loosened, all write endpoints (`/setup`, `/hardware`,
`/call`, `/pb_add`, `/pb_del`) become CSRF-able.

**Remediation:** Issue a per-session CSRF token embedded in forms and validated
on POST; make logout a POST.

---

### SEC-05 — Factory credentials and open setup AP (Medium, partly by design)

**Where:** `components/config_store/app_config.h:241-242`;
`main/wifi_manager.c:84-96`; warning text `ui_controller.c:759-763`.

**Description:** The setup AP is `WIFI_AUTH_OPEN` with a well-known SSID and the
factory login is `admin`/`esp32sip`. The code mitigates this by hiding web
credential editing while in AP mode and documenting a password change, but an
attacker in radio range can still reach the login page (DNS-hijacked by the
device itself) and race the legitimate owner to any SIP/Wi-Fi settings, or brute
the default login (throttled to 5/30 s).

**Remediation:** Generate a random per-device setup password/PSK printed on a
label, or provision via Bluetooth/serial; at minimum force a web-password change
before accepting SIP credentials.

---

### SEC-06 — RTP not validated (Medium)

**Where:** `main/rtp_handler.c:174-217`, drain at `main/audio_pipeline.c:453-467`.

**Description:** `rtp_receive_packet` accepts any UDP datagram with RTP version
2 from any source, and neither the SSRC nor the source address/port is checked
against the negotiated remote. An attacker can inject arbitrary audio into the
speaker and consume jitter-buffer slots.

**Remediation:** Pin the RTP remote to the SDP-negotiated address (and to the
observed symmetric source), verify `ssrc` and payload type, and drop packets
outside a small sequence window.

---

### SEC-07 — Digest nonce count fixed (Low)

**Where:** `main/sip_client.c:170` (`const char *nc = "00000001";`).

**Description:** With `qop=auth`, RFC 2617 requires the `nc` counter to
increment for each use of a nonce. It is constant here. Reusing a nonce with the
same `nc` weakens replay resistance and some servers reject it. A fresh
`cnonce` is generated per challenge (`216-218`), so practical impact is limited.

**Remediation:** Increment and persist `nc` per nonce.

---

### SEC-08 — Login lockout and comparisons (Low)

**Where:** `main/ui_controller.c:55-58, 787-841`.

**Description:** `s_login_fails`/`s_login_blocked_until` are global, so any
attacker can lock out legitimate users (DoS) with 5 bad attempts per 30 s.
Credentials are compared with `strcmp` (no constant-time compare), and failed
attempts are logged with the attempted username.

**Remediation:** Per-source throttling, constant-time comparison, and avoid
logging attempted usernames.

---

### SEC-09 — SIP responses not correlated (Low)

**Where:** `main/sip_client.c:1295-1441`.

**Description:** Responses are matched only by `CSeq` method + `Call-ID`. The
`Via` branch and source address are ignored, so spoofed responses could alter
state (e.g., a forged `200 OK`/`BYE`). Combined with SEC-01 this is easier.

**Remediation:** Validate the top `Via` branch and the source of responses.

---

### FUN-01 — Hostname resolution is broken (High)

**Where:** `main/sip_client.c:306-315` (`sip_client_init`),
`793-809` (`resolve_server`), `577-584` (STUN).

**Description:** All three call `dns_gethostbyname(name, &addr, NULL, NULL)` and
then treat anything other than `ERR_OK` as failure, falling back to
`ipaddr_aton()`. lwIP's `dns_gethostbyname` is **non-blocking**: for a name that
is not already cached it returns `ERR_INPROGRESS` and only delivers the result
later via the (here `NULL`) callback. Consequently:

* `sip_client_init()` returns `NULL` for any hostname server
  (`sip.provider.com`) unless it is already in the DNS cache → **no SIP client
  at all**.
* `sip_client_reload()` can never resolve a hostname server.
* STUN with the configured hostname `stun.l.google.com` silently never runs.

The README explicitly advertises "server (IP or domain)" (README.md:90), so
this is a stated-feature failure. Literal IP addresses work.

**Remediation:** Use the blocking `getaddrinfo()` (or `esp_netif` DNS) from the
SIP task with a timeout, or implement `dns_gethostbyname` with a callback and a
FreeRTOS semaphore/event wait. Then re-check the resolved address.

---

### FUN-02 — No outgoing CANCEL (High)

**Where:** `main/sip_client.c:478-501` (`do_hangup`), `1099-1131` (`send_bye`),
`707-719` (Timer B).

**Description:** Terminating an in-progress outgoing call calls `do_hangup`,
which calls `send_bye`. `send_bye` begins with
`if (client->call_state < SIP_CALL_STATE_CONNECTING) return;`. In `INVITING`
the function returns immediately, so **nothing is sent**; the code then forces
`call_state = IDLE` and fires `on_call_ended`. The pending INVITE transaction is
left dangling: no `CANCEL`, and a later `200 OK` is dropped because
`current_call_id` has been cleared. Timer B (`sip_client.c:707-719`) likewise
abandons the transaction with no CANCEL. The `Allow`/`User-Agent` headers and
README advertise CANCEL support.

**Remediation:** Implement `send_cancel()` (same `Call-ID`, `From` tag, `CSeq`
as the INVITE, a new branch) and invoke it when terminating in `INVITING`.

---

### FUN-03 — Retransmitted INVITE rejected as busy (High)

**Where:** `main/sip_client.c:1456-1462`.

**Description:** On a request `INVITE`, if `call_state != IDLE` the device replies
`486 Busy Here`. When the device has already accepted an INVITE and is in
`INCOMING`/`RINGING`, a normal UDP retransmission of the same INVITE (same
Call-ID) hits this path and is answered `486`, causing the caller to abandon a
call the device intended to accept. This makes calls unreliable on any lossy
link.

**Remediation:** If the incoming INVITE matches the current dialog
(same `Call-ID` + `CSeq`), re-send the previous provisional/final response
instead of `486`. Keep a small server-transaction cache.

---

### FUN-04 — REGISTER has no retransmission/timeout (Medium)

**Where:** `main/sip_client.c:889-938` (build/send), `1322-1355` (response).

**Description:** A REGISTER is sent once at startup and the refresh timer is only
armed after a `200 OK` (`1330`). If the initial REGISTER (or a refresh) is lost,
there is no SIP Timer E/F retransmission; the device can remain unregistered
indefinitely until reboot. Auth challenges are handled, but only in response to
a received challenge.

**Remediation:** Add transaction retransmission with backoff, and arm/adjust the
refresh timer independently of the response.

---

### FUN-05 — REGISTER `From` tag unstable (Medium)

**Where:** `main/sip_client.c:920`.

**Description:** The REGISTER `From` header tag is
`client->current_from_tag[0] ? client->current_from_tag : "reg"`. After any
call, `current_from_tag` holds the *call's* tag, so the registration identity
changes across refreshes. `Call-ID` (`reg_call_id`) is stable, so this is not
fatal, but it is incorrect and can confuse some registrars.

**Remediation:** Use a dedicated, stable registration tag generated at init.

---

### FUN-06 — Data race on shared settings (Medium)

**Where:** `main/ui_controller.c:1368` (`*g_settings = updated;`) consumed by
`main/sip_client.c:812-885` (`apply_pending_reload`) and read live at
`1494-1499` (device role / auto-answer delay) and `main/audio_pipeline.c:221-223`.

**Description:** The HTTP task overwrites the whole `app_settings_t` by struct
assignment while the SIP task may be reading fields (`device_role`,
`auto_answer_delay_s`) or copying strings (`user`, `password`, `domain`). A
non-atomic struct copy concurrent with reads can yield torn/partially-updated
strings, and the SIP task can observe a half-written credential set. The
`reload_pending` flag orders *intent* but not the data.

**Remediation:** Protect `app_settings_t` with a mutex; hand a private snapshot
to the SIP task via a queue/worker, or double-buffer with an atomic pointer
swap.

---

### FUN-07 — AP-fallback race at boot (Medium)

**Where:** `main/main.c:145-170`; `main/wifi_manager.c:115-139`.

**Description:** `wifi_init_sta()` returns as soon as `esp_wifi_start()` is
issued; the AP fallback (`start_ap_mode()`) only happens later, on
`WIFI_EVENT_STA_DISCONNECTED` after `WIFI_MAX_RETRY` failures. `main.c` tests
`wifi_is_ap_mode()` immediately (`147`) and, when it is still `false`, proceeds
to create the audio pipeline and SIP client against an address that may never
appear. When AP mode then starts, the SIP task (which waits for
`IP_ACQUIRED_BIT`) wakes with the AP IP and tries to register against the
configured (unreachable) server, while the web server is already running in
"normal" mode. The system ends up in a mixed state rather than a clean captive
portal.

**Remediation:** Have `wifi_manager` signal a settled outcome (STA-up or AP-up)
via the event group and block `app_main` on it before choosing the init path.

---

### FUN-08 — Non-serialized I2C access (Medium)

**Where:** `main/codec_driver.c:94-98` (writes), `232-252`
(`codec_set_volume`), `254-268` (`codec_set_mic_gain`).

**Description:** `codec_set_volume()` can be invoked from the HTTP task
(`ui_controller.c:1371, 1479`) while init/other I2C users run. The legacy
`i2c_master_write_to_device` is not reentrant across tasks; interleaved
transactions can corrupt codec registers.

**Remediation:** Guard codec register access with a mutex, or route volume
changes through the audio task.

---

### FUN-09 — Ignored pipeline-start failure (Medium)

**Where:** `main/sip_client.c:1287-1293`; `main/audio_pipeline.c:296-312`.

**Description:** `audio_pipeline_start()` can fail after it has already switched
the I2S rate and unmuted the DAC (e.g., `rtp_session_create` fails), returning
`ESP_FAIL`. `start_call_audio()` ignores the result, so the SIP dialog is set
`ACTIVE` with no RTP session — silent call, no error surfaced. Also, on that
failure AEC state allocated earlier is not torn down.

**Remediation:** Propagate the failure, answer `488`/`503` or drop the call,
and make `audio_pipeline_start` transactional (roll back on error).

---

### FUN-10 — Hardware page touch pins ignored (Medium)

**Where:** `components/board_hal/touch_driver.c:60, 73-78` vs
`main/ui_controller.c:1663-1666`.

**Description:** The web *Hardware → Touch* tab exposes `touch_cs` and
`touch_irq` and persists them, but `touch_driver_init()` uses the compile-time
`TOUCH_CS`/`TOUCH_IRQ` macros directly and never loads `hw_settings`. Changing
them in the UI does nothing (the device still reboots, so it *looks* applied).

**Remediation:** Load `hardware_settings_t` and prefer its values when set,
mirroring `display_tft_init()`.

---

### FUN-11 — Matrix keypad is dead code (Low)

**Where:** `components/board_hal/keypad.c`; `keypad_init()` only at
`main/main.c:122`; `keypad_get_key()` has no caller.

**Description:** The 4×4 matrix keypad is initialized but never scanned, so
physical keypad dialing advertised in the README (lines 26, 249) does not exist.
`keypad_get_key()` also blocks (waits for release) and would be unsafe to call
from the HTTP/SIP tasks as written.

**Remediation:** Either wire keypad scanning into a task that feeds the dialer,
or remove the code and README claim.

---

### FUN-12 — Suspicious timeout arithmetic (Low)

**Where:** `main/audio_pipeline.c:397, 432`.

**Description:** `pdMS_TO_TICKS(AUDIO_FRAME_MS * 2) * portTICK_PERIOD_MS` mixes
ticks and milliseconds. With the configured 1 kHz tick it equals 40 ticks
(correct), but with any other `CONFIG_FREERTOS_HZ` it is wrong by the tick
period. The `UINT32_MAX` timeout at `i2s_channel_write` (`500-501`) also blocks
indefinitely on a stalled DMA.

**Remediation:** Pass `pdMS_TO_TICKS(40)` directly and use a finite write
timeout.

---

### FUN-13 — RTP/NAT limitations (Low)

**Where:** `main/rtp_handler.c:42-111`; `main/sip_client.c:572-636`,
`990-1004`.

**Description:** STUN maps only the SIP socket (`sip_client.c:590-593`), while
RTP uses a separate fixed port `RTP_LOCAL_PORT_BASE = 16384`
(`app_config.h:94`) advertised in SDP. The public RTP mapping is never
discovered, and there is no symmetric-RTP/comedia latching, so RTP rarely
traverses NAT even though the signalling may. The claim of NAT traversal is
therefore partial.

**Remediation:** Perform STUN on the RTP socket too (or receive the first RTP
packet to latch), and advertise the discovered mapping in SDP.

---

### FUN-14 — `ESP_ERROR_CHECK` aborts on display wiring errors (Low)

**Where:** `components/board_hal/display_tft.c:97-128`.

**Description:** Any panel bus/init failure calls `ESP_ERROR_CHECK`, aborting the
firmware. A mis-wired board or an absent panel at the web-configured pins
results in a boot loop rather than a degraded (audio-only) device.

**Remediation:** Log and continue without the panel; let LVGL skip rendering.

---

### FUN-15 — OPUS codec can silently zero audio (Low)

**Where:** `components/audio_pipeline/opus_codec.c:72-91`; PT gate
`main/sip_client.c:228-246, 960-1005`.

**Description:** When `USE_CODEC_OPUS` is defined but `<opus.h>` is not linked,
the wrapper is a no-op: encode returns 0 and decode writes 960 zeros. The SDP
layer still offers PT 96 (`append_rtpmap`), so the device can negotiate OPUS it
cannot actually carry. Default build has OPUS disabled, so this only bites the
PRO tier without the optional libopus dependency.

**Remediation:** Compile OPUS out entirely when libopus is absent so PT 96 is
not advertised.

---

### FUN-16 — Header search not line-anchored (Low)

**Where:** `main/sip_client.c:1194-1224` (`ci_strstr`, `parse_header`).

**Description:** `parse_header` finds the first occurrence of a header name
anywhere in the message (including inside another header's value or the body),
not at the start of a line. Low practical risk but not RFC-correct; a crafted
message could confuse parsing.

**Remediation:** Parse headers line-by-line.

---

### CON-01 — Audio stop can free RTP session under a blocked task (High)

**Where:** `main/audio_pipeline.c:315-342` (`audio_pipeline_stop`),
`380-411` (stop ack), `500-501` (`i2s_channel_write(..., UINT32_MAX)`).

**Description:** `audio_pipeline_stop()` clears `is_running`, waits up to 500 ms
for `stop_ack_sem`, then deletes `p->rtp_session`. The audio task only posts the
ack *after* finishing its current iteration. If the task is blocked inside
`i2s_channel_write` with `UINT32_MAX` (stalled DMA) or in a long I2S read, the
500 ms wait times out and `rtp_session_delete()` frees the session while the
audio task may still dereference it → use-after-free. The binary semaphore is
not reset between calls, so an extra/leftover post can also let `stop()` return
before the task actually leaves the running path.

**Remediation:** Use a proper task-notification/handshake that cannot time out
silently (or never free while `is_running` may still be observed true), add
finite I2S timeouts, and reset the ack semaphore at each `start`.

---

### CON-02 — Jitter-buffer counter desync (Low)

**Where:** `main/rtp_handler.c:226-297`.

**Description:** `valid_packets_count` is incremented on insert and decremented
on play, but a duplicate overwrite of an already-valid slot does not increment,
and `next_playout_seq` advances on misses. Under heavy reordering/wrap the
counter can drift from the true occupancy, affecting the pre-roll heuristic
(`is_buffering`) and possibly stalling playback. The drain loop
(`audio_pipeline.c:460-467`) also keeps receiving while packets arrive, briefly
unbounded per 20 ms frame.

**Remediation:** Derive occupancy from the buffer contents (or bound the drain
loop), and reset the buffer on each new call.

---

### BLD-01 — CI cannot build the current source (High)

**Where:** `.github/workflows/build.yml:26` pins `esp_idf_version: v5.1.4`;
`main/CMakeLists.txt:32` lists `esp_driver_i2s` in `REQUIRES`.

**Description:** The `esp_driver_i2s` component only exists from IDF 5.4 / 6.x;
in 5.1.4 the I2S driver lives in the monolithic `driver` component. CMake will
fail to resolve `esp_driver_i2s` under 5.1.4, so the advertised green CI
cannot be reproducing the checked-in build (which was produced with **IDF
6.1.0**, per `build_check/config.env` and `sdkconfig`). The README's "v4.4+"
claim (line 41) is also impossible: the code uses the IDF 5.x
`driver/i2s_std.h` API.

**Remediation:** Pin CI to the supported IDF (6.1.x, or 5.4+) and align README,
or drop `esp_driver_i2s` from `REQUIRES` and document the minimum.

---

### BLD-02 — Build/version metadata inconsistent (Medium)

**Where:** README.md:41 (`ESP-IDF v4.4, v5.0, v5.1+`), `sdkconfig` (6.1.0),
`dependencies.lock` (`idf 6.1.0`), CI (5.1.4).

**Description:** Four different IDF expectations coexist. Contributors cannot
tell which toolchain is authoritative.

**Remediation:** State one supported range and enforce it in CI; commit a
`idf_component.yml` `idf: ">=5.4"` constraint.

---

### BLD-03 — WakeNet model partition missing (Medium)

**Where:** `partitions.csv` (no `model` partition); `sdkconfig.defaults:19`
(`CONFIG_MODEL_IN_SPIFFS=y`); `app_config.h:54` (`USE_WAKE_WORD 0`);
`components/audio_pipeline/idf_component.yml` (esp-sr dependency).

**Description:** esp-sr is always pulled in and `MODEL_IN_SPIFFS` is enabled,
but the 4 MB partition table has no `model` partition. If a user flips
`USE_WAKE_WORD` to 1 (as the README suggests for 8 MB boards), the model
partition does not exist and `esp_srmodel_init("model")` fails at runtime
(`wake_word.c:25-29`). The dependency also bloats every build even when wake
word is off.

**Remediation:** Provide an 8 MB partition table variant with the `model`
partition and make the esp-sr dependency conditional on `USE_WAKE_WORD`.

---

### DOC-01 — Documentation overstates features (Low)

**Where:** README.md lines 14 (CANCEL), 26 (matrix keypad), 193 ("Full-Duplex
AEC … esp-sr").

**Description:** CANCEL is not sent by the client (FUN-02); the matrix keypad is
never scanned (FUN-11); "Full-Duplex AEC (esp-sr)" hints at hardware AEC that is
not integrated — only the software NLMS filter in `aec_filter.c` exists. This
misleads users evaluating the device.

**Remediation:** Correct the README or implement the features.

---

## 5. Positive observations

* **Heap-safety discipline:** almost all network/string copies are bounded
  (`strncpy` with explicit sizes, `snprintf`, `pg_escape` for all HTML output,
  `form_get` rejects over-length values instead of truncating).
* **Stack-overflow fix:** every SIP action is deferred to `sip_task`
  (`sip_client.c:34-40, 384-388`), avoiding the earlier 2 KB-task overflow.
* **Single-writer discipline for the socket:** only `sip_task` touches the SIP
  socket and builds messages — a sound design once the shared settings race
  (FUN-06) is fixed.
* **Robust login throttle** that never blocks the single HTTP task
  (`ui_controller.c:52-58`).
* **Codec correctness:** the G.711 tables and G.722 ADPCM follow the public-
  domain references; AEC is a genuine NLMS filter with an anti-divergence gate.
* **Web UI never uses external assets or JS**, and output is HTML-escaped.
* **Graceful hardware fallbacks:** audio output Auto-detect
  (`codec_driver.c:167-220`), TX-only operation when no mic is wired
  (`audio_pipeline.c:514-532`), and I2S TX mute between calls.

---

## 6. Prioritized remediation roadmap

**P0 — before any deployment on an untrusted network**
1. SEC-01: authenticate inbound SIP / gate auto-answer.
2. FUN-01: fix DNS resolution (blocking `getaddrinfo` or callback + wait).
3. SEC-02 + SEC-03: HTTPS for the web UI; flash/NVS encryption for secrets.

**P1 — call reliability**
4. FUN-02: implement CANCEL; FUN-03: handle INVITE retransmissions.
5. CON-01: make the audio stop/teardown handshake safe (no UAF).
6. FUN-04/FUN-05: REGISTER retransmission + stable `From` tag.

**P2 — correctness & build**
7. FUN-06/FUN-08: lock shared settings and I2C access.
8. FUN-07: settle Wi-Fi mode before initialising SIP/audio.
9. FUN-09/FUN-10: propagate pipeline failures; apply touch pins.
10. BLD-01/BLD-02/BLD-03: align CI, IDF version, partition table.

**P3 — hygiene**
11. SEC-04/SEC-06/SEC-07/SEC-08/SEC-09, FUN-11..FUN-16, CON-02, DOC-01.

---

## 7. Appendix — evidence index

| File | Notable lines |
|:--|:--|
| `main/sip_client.c` | 142-219 digest; 268-341 init/DNS; 478-501 hangup; 707-719 Timer B; 793-885 reload; 889-1005 REGISTER/INVITE/SDP; 1099-1188 BYE/response; 1194-1285 parsing; 1295-1585 incoming handler |
| `main/ui_controller.c` | 40-151 sessions; 787-841 login; 1251-1437 settings POST; 1439-1485 actions/volume; 1580-1778 hardware; 1786-1825 server+routes |
| `main/audio_pipeline.c` | 194-358 lifecycle; 362-506 audio task; 510-657 I2S helpers |
| `main/rtp_handler.c` | 42-111 session; 130-172 send; 174-217 receive; 226-298 jitter buffer |
| `main/wifi_manager.c` | 23-75 DNS hijack; 77-103 AP mode; 105-140 events; 142-168 STA init |
| `main/main.c` | 111-211 init and task wiring |
| `components/config_store/config_manager.c` | 52-190 NVS load/save |
| `components/board_hal/touch_driver.c` | 59-123 (compile-time pins) |
| `components/board_hal/keypad.c` | 19-54 (no caller) |
| `components/audio_pipeline/aec_filter.c` | 32-87 NLMS |
| `components/audio_pipeline/opus_codec.c` | 28-93 optional libopus |
| `partitions.csv`, `sdkconfig.defaults`, `.github/workflows/build.yml` | build/config facts |

### Severity scale used

* **Critical** – remote unauthenticated compromise of privacy/safety.
* **High** – serious security weakness, feature-breaking bug, or crash/UAF.
* **Medium** – reliability/robustness defect or limited-impact weakness.
* **Low** – correctness/hygiene issues with limited impact.

---

*End of audit. Findings are based on revision `8a8900b`; re-verify line numbers
if the source has moved.*
