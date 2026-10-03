# Feature Implementation Workflow

How to take a feature from [`FEATURE-PLAN.md`](FEATURE-PLAN.md) to a merged,
verified change. One milestone task per branch/PR; each task passes the same
gates. This is the playbook the `feature-implementer` agent and `/feature` /
`/verify` commands follow.

Related: [`FEATURE-PLAN.md`](FEATURE-PLAN.md) · [`AUDIT.md`](AUDIT.md) ·
[`TOOLING.md`](TOOLING.md)

---

## 1. Roles & tooling

| Tool | Used for | Where |
|:--|:--|:--|
| `feature-implementer` agent | implements one task end-to-end | `.opencode/agent/feature-implementer.md` |
| `/feature <task>` command | kicks off a task (runs as a subagent) | `.opencode/command/feature.md` |
| `/verify` command | runs the quality gates for the current change | `.opencode/command/verify.md` |
| **esp32 skill** | pin validation, chip/LVGL/IDF references | `.opencode/skills/esp32/` |
| **esp-mcp** | `set-target`, `build`, `flash`, `list ports`, `pytest` | `tools/esp-mcp/` + `opencode.json` |
| Wokwi | MAX7219 + preview simulation | `wokwi/`, `wokwi.toml` |
| PC simulator | LVGL UI preview | `simulator/` |
| Audit | guardrails / review checklist | `docs/AUDIT.md` |

> Restart opencode after any config/skill/agent change.

---

## 2. Branching & commits

* Branch per task: `feat/m1-hub75-bringup`, `feat/m3-mqtt-source`,
  `fix/…`, `docs/…`.
* **Conventional Commits**: `feat(matrix): add MAX7219 single-line backend`.
* One logical change per commit; never commit generated artifacts
  (`build*/`, `sdkconfig`, `managed_components/`, `tools/esp-mcp/.venv/`).
* Feature work is **opt-in** behind Kconfig (`CONFIG_SIP_MATRIX_HUB75`,
  `CONFIG_SIP_MATRIX_MAX7219`) so `main` always builds and boots without the
  hardware.

---

## 3. Definition of Ready (DoR)

A task may start only when all are true:

- [ ] It maps to a milestone task in `FEATURE-PLAN.md`.
- [ ] Interfaces are named (C API, struct fields, NVS keys, HTTP routes).
- [ ] Pin map / defaults decided (validate with the esp32 skill).
- [ ] Failure modes + concurrency impact identified (see §6).
- [ ] Test approach chosen (Wokwi / simulator / hardware / web preview).

If any is missing, the agent stops and asks rather than guessing.

---

## 4. The loop

```
0 Intake (DoR)  →  1 Design freeze  →  2 Branch + scaffold
   → 3 Implement vertical slice  →  4 Static validate (skill + build)
   → 5 Functional test (Wokwi / hardware / web)  →  6 Review (audit gates)
   → 7 Docs  →  8 Ship (PR + CI)
```

1. **Design freeze** — write the interface first (header + settings fields +
   route names). No implementation until it compiles conceptually.
2. **Scaffold** — create files, register in CMake/`idf_component.yml`, add
   settings keys (defaults + load + save), add the web tab/route stub.
3. **Vertical slice** — make one path work end-to-end (e.g. test pattern →
   caller ID → queue → alert), not all backends at once.
4. **Static validate** — run the gates in §5.
5. **Functional test** — capture evidence (serial log, web screenshot, Wokwi).
6. **Review** — walk §6 guardrails explicitly.
7. **Docs** — update `FEATURE-PLAN.md` status, `docs/`, README if user-facing.
8. **Ship** — PR with the DoD checklist; CI must be green.

---

## 5. Quality gates

| Gate | Check | Command / method |
|:--|:--|:--|
| **G0** | DoR complete | review §3 |
| **G1** | Interfaces frozen | diff headers/structs |
| **G2** | Pin map valid | `python .opencode/skills/esp32/scripts/validate_pinmap.py --format json < pins.json` |
| **G3** | Compiles, no new warnings | `uv --directory tools/esp-mcp run main.py` tool `build_esp_project`, or `idf.py build` |
| **G4** | Functional evidence | Wokwi / hardware serial log / web preview |
| **G5** | Audit guardrails clear | §6 checklist |
| **G6** | Docs + CI green | PR checks |

Minimum per task: **G2 + G3 + G5**; hardware-touching tasks also need **G4**.

### Build via esp-mcp (or plain idf.py)

```bash
# set target then build (esp-mcp tools: setup_project_esp_target, build_esp_project)
idf.py set-target esp32s3
idf.py build
# or with the matrix profile:
idf.py -B build_s3 build
```

### Simulate (no hardware)

```bash
# MAX7219 + preview logic in Wokwi
./wokwi/build.ps1        # or build.sh
# then: F1 -> "Wokwi: Start Simulator"
```

---

## 6. Guardrails (from the audit — must hold for every change)

- [ ] **State-only callbacks.** `notification_on_incoming()` runs in `sip_task`;
      it only sets state / signals — never draws or blocks. (CON-01, FUN-09)
- [ ] **Dedicated matrix task** at prio < audio (10) and SIP (8); all drawing
      there, under a mutex.
- [ ] **Settings snapshot.** New settings are read from a snapshot, never from
      the live `*g_settings` mutated by the HTTP task. (FUN-06)
- [ ] **Bounded buffers** on every `strncpy`/`snprintf`/form field; reject
      over-length input rather than truncating.
- [ ] **Authenticated transports** for queue/alert (MQTT TLS/user-pass, HTTP
      token, AMI secret) — no cleartext over untrusted networks. (SEC-02/03)
- [ ] **No inbound trust.** Alert/queue data must not enable actions beyond
      display; SIP auth gaps remain documented. (SEC-01)
- [ ] **Opt-in Kconfig.** Feature off ⇒ identical behaviour to today.
- [ ] **Web output escaped** (`pg_escape`) and routes behind `require_login`.
- [ ] **No new blocking** on the HTTP or SIP tasks.

---

## 7. Milestone execution order

Implement in this order so each layer is testable before the next depends on it:

| # | Milestone | Depends on | Primary evidence |
|:--|:--|:--|:--|
| M1 | Matrix bring-up (HUB75 + MAX7219 + font) | — | test pattern on panel / Wokwi |
| M2 | Caller ID | M1 | scroll on incoming call |
| M3 | Queue mode (MQTT/HTTP/AMI/local) | M1, M2 | count updates from each source |
| M4 | Alert mode (button/touch/web/timeout) | M1, M2 | ack clears alert |
| M5 | Polish (dual backend, multi-panel, ARI) | M3, M4 | — |

### Task tracker

| ID | Task | Status |
|:--|:--|:--|
| M1.1 | `matrix_display` skeleton + C API + font5x7 | todo |
| M1.2 | `matrix_hub75.cpp` (esp-hub75) + `idf_component.yml` | todo |
| M1.3 | `matrix_max7219.c` single-line backend | todo |
| M1.4 | S3 pin-map Kconfig blocks in `app_config.h` | todo |
| M1.5 | `hardware_settings_t` matrix pins + Hardware→Matrix tab | todo |
| M1.6 | Test pattern + brightness | todo |
| M1.7 | Wokwi MAX7219 part + ticker | todo |
| M2.1 | `notification_service` + `main.c` wiring | todo |
| M2.2 | `phonebook_find_by_uri()` + room resolution | todo |
| M2.3 | Scrolling caller text | todo |
| M2.4 | Web preview (`matrix_get_ascii`) + Display tab | todo |
| M3.1 | `queue_state_t` + queue task + hot-swap | todo |
| M3.2 | MQTT source | todo |
| M3.3 | HTTP source | todo |
| M3.4 | PBX/AMI source | todo |
| M3.5 | Local counting source | todo |
| M3.6 | Queue rendering + thresholds + Queue tab | todo |
| M4.1 | Alert state machine + blink/tone/re-alert | todo |
| M4.2 | Button / touch / web ack + timeout | todo |
| M4.3 | Missed-alert log + banner + Alert tab | todo |
| M5.1 | Mono auto-detect / channels | todo |
| M5.2 | Multi-panel chaining | todo |
| M5.3 | Optional ARI variant | todo |

Update a row to `in-progress` when its branch opens and `done` only after G6.

---

## 8. Definition of Done

- [ ] All gates G0–G6 passed; evidence linked in the PR.
- [ ] Feature works with the flag **off** (no regression) and **on**.
- [ ] Settings persist across reboot (NVS round-trip).
- [ ] No new compiler warnings; `idf.py build` clean for esp32s3.
- [ ] `docs/` and `FEATURE-PLAN.md` updated; user-facing changes in README.
- [ ] Audit guardrails ticked in the PR description.

---

## 9. Escalation & rollback

* Blocked > 30 min on hardware behaviour → stop, capture logs, ask.
* A change that destabilises `main` → revert the merge, keep the branch, reopen.
* HUB75 + Wi-Fi interference or SRAM pressure → fall back to the `preview`
  backend, ship the logic, revisit hardware separately.

---

## 10. Command reference

| Command | Action |
|:--|:--|
| `/feature M1.1 matrix_display skeleton` | start a task (subagent follows this doc) |
| `/verify` | run G2/G3/G5 for the working tree |
| `python .opencode/skills/esp32/scripts/validate_pinmap.py` | pin validation |
| esp-mcp `build_esp_project` | compile |
| esp-mcp `flash_esp_project` | flash (confirm first) |
| `./wokwi/build.ps1` + `Wokwi: Start Simulator` | simulate MAX7219/preview |
