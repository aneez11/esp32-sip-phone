# Project AI Tooling

Two external tools are installed for this project. Both are third-party and
pinned to the versions noted below; neither is required to build the firmware.

| Tool | Type | Source | Installed at |
|:--|:--|:--|:--|
| **esp32** skill | opencode skill (reference + validation) | [`ezrover/ESP32-AI-Agent-Skill`](https://github.com/ezrover/ESP32-AI-Agent-Skill) (MIT) | `.opencode/skills/esp32/` |
| **esp-mcp** | opencode MCP server (ESP-IDF commands) | [`horw/esp-mcp`](https://github.com/horw/esp-mcp) (PoC) | `tools/esp-mcp/` + `opencode.json` |

> Restart opencode after installing — config and skills are loaded once at
> startup and are not hot-reloaded.

## 1. ESP32 skill

`.opencode/skills/esp32/` contains:

```
SKILL.md            the skill (auto-activates on ESP32/IDF/PlatformIO/LVGL/Waveshare topics)
references/         per-chip GPIO databases, LVGL v8.2–v9.5, Waveshare boards, protocols
scripts/            validate_pinmap.py, generate_config.py (+ scripts/platforms/)
LICENSE             upstream MIT license
```

It gives the agent expert ESP32 pin-safety knowledge (GPIO12 flash-voltage trap,
ADC2/Wi-Fi conflicts, input-only pins, flash/PSRAM reservations) and can generate
Arduino/ESP-IDF init boilerplate. The Python scripts are run on demand:

```bash
# Validate a pin map (esp32/esp32s2/esp32s3/esp32c3/esp32c6)
echo '{"platform":"esp32s3","wifi_enabled":true,"pins":[{"gpio":21,"function":"I2C_SDA","protocol_bus":"i2c"}]}' \
  | python .opencode/skills/esp32/scripts/validate_pinmap.py --format json

# Generate ESP-IDF boilerplate
python .opencode/skills/esp32/scripts/generate_config.py --format text --framework espidf < input.json
```

Only `name` and `description` frontmatter are used by opencode; the upstream
`version` field is ignored. The one adaptation made at install time: the
SKILL.md note now says paths are relative to the skill's own directory (opencode
treats them that way) instead of "the plugin's root directory".

## 2. esp-mcp (MCP server)

`tools/esp-mcp/` vendors `main.py`, `esp_utils.py`, `pyproject.toml`, `uv.lock`
and `.python-version` (upstream `.python-version` was relaxed from `3.11.0` to
`3.11` so uv can use any installed 3.11.x patch). `opencode.json` registers it:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "mcp": {
    "esp-mcp": {
      "type": "local",
      "command": ["uv", "--directory", "tools/esp-mcp", "run", "main.py"],
      "enabled": true,
      "environment": {
        "IDF_PATH": "C:/esp/v6.1/esp-idf",
        "PATH": "C:/Program Files/Git/bin;{env:PATH}"
      }
    }
  }
}
```

Tools it exposes: `build_esp_project`, `setup_project_esp_target`,
`create_esp_project`, `flash_esp_project`, `list_esp_serial_ports`,
`run_esp_idf_install`, `run_pytest`.

### Prerequisites

* **`uv`** on PATH (used to run the server in its own venv).
* **ESP-IDF** at `IDF_PATH` (default above is `C:/esp/v6.1/esp-idf`; change it
  to your install).
* **`bash`** — the server shells out with
  `bash -c "source $IDF_PATH/export.sh && idf.py …"`. On Windows this is the
  **Git Bash** `bash.exe`; the config prepends `C:/Program Files/Git/bin` to
  `PATH` so it is found. If `bash` is unavailable, run opencode from WSL/Linux
  instead, or disable the server.

> The server runs real `idf.py build` / `flash` / `install` commands — treat it
> as trusted local code and review tool calls before approving flashing.

### Disable / remove

* Disable without deleting: set `"enabled": false` for `esp-mcp` in
  `opencode.json`.
* Remove the skill: delete `.opencode/skills/esp32/`.
* Remove the server: delete `tools/esp-mcp/` and the `mcp.esp-mcp` entry.

## Provenance / updates

Both were cloned at install time from their `main` branches (2026-10-03) and are
vendored in-repo so they work offline. Re-run the clone/copy steps to update.
The skill's scripts and the MCP server are not invoked during firmware builds.
