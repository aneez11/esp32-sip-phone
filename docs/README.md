# ESP32-SIP-Voice — Documentation

This folder holds engineering documentation for the project. It is **not**
firmware; nothing here is compiled or flashed.

## Index

| Document | Description |
|:--|:--|
| [`AUDIT.md`](AUDIT.md) | Full system audit (architecture, security, SIP/RTP correctness, concurrency, build/CI, documentation accuracy) with a prioritized remediation roadmap. |
| [`FEATURE-PLAN.md`](FEATURE-PLAN.md) | Planned matrix displays (HUB75 64×32 and 8×8 MAX7219 single-line array), queue mode and alert mode: hardware, drivers, component architecture, settings schema, milestones. |
| [`TOOLING.md`](TOOLING.md) | Installed project AI tooling: the `esp32` skill and the `esp-mcp` MCP server (prerequisites, usage, removal). |
| [`../wokwi/README.md`](../wokwi/README.md) | Wokwi simulation: build script, board/display wiring, what can and cannot be tested. |

## Audit at a glance

The audit of revision `8a8900b` found 1 critical, 6 high, 10 medium and 14 low
issues. The highest-priority items are:

1. **SEC-01 (Critical)** — inbound SIP is unauthenticated and Speaker mode
   auto-answers, allowing anyone on the LAN to open the microphone.
2. **FUN-01 (High)** — hostname DNS resolution is broken, so domain-based SIP
   servers and STUN silently fail.
3. **FUN-02 / FUN-03 (High)** — outgoing calls cannot be cancelled and
   retransmitted INVITEs are rejected as busy.
4. **SEC-02 / SEC-03 (High)** — plaintext web UI and unencrypted credentials.
5. **BLD-01 (High)** — CI (IDF v5.1.4) cannot build the current source
   (requires `esp_driver_i2s`, IDF ≥ 5.4).

See [`AUDIT.md`](AUDIT.md) for evidence, file/line references and remediation
guidance.
