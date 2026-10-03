#!/usr/bin/env bash
# Build the Wokwi simulation firmware into build_wokwi/.
# Run from a shell with the ESP-IDF environment exported (. $IDF_PATH/export.sh).
#
#   ./wokwi/build.sh                    # build (esp32)
#   CLEAN=1 ./wokwi/build.sh            # wipe build_wokwi/ first
#   ./wokwi/build.sh esp32s3            # other target
#
# It uses an isolated sdkconfig (build_wokwi/sdkconfig) seeded from
# sdkconfig.defaults + sdkconfig.wokwi.defaults, so normal builds are untouched.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TARGET="${1:-esp32}"

# NOTE: SDKCONFIG must be passed as a CMake variable (-D), not an environment
# variable, otherwise ESP-IDF writes the Wokwi options into the root sdkconfig.
COMMON=(
    -B build_wokwi
    -DSDKCONFIG=build_wokwi/sdkconfig
    "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.wokwi.defaults"
)

if [[ "${CLEAN:-0}" == "1" ]]; then
    echo "Removing build_wokwi/ ..."
    rm -rf build_wokwi
fi

echo "Target: $TARGET  (Wokwi simulation build)"
idf.py "${COMMON[@]}" set-target "$TARGET"
idf.py "${COMMON[@]}" build

echo
echo "Done. Firmware: build_wokwi/flasher_args.json"
echo "Open this folder in VS Code and press F1 -> 'Wokwi: Start Simulator'."
