# Build the Wokwi simulation firmware into build_wokwi/.
# Run from an "ESP-IDF PowerShell" so `idf.py` is on PATH.
#
#   ./wokwi/build.ps1              # build (esp32)
#   ./wokwi/build.ps1 -Clean       # wipe build_wokwi/ first
#   ./wokwi/build.ps1 -Target esp32s3
#
# It uses an isolated sdkconfig (build_wokwi/sdkconfig) seeded from
# sdkconfig.defaults + sdkconfig.wokwi.defaults, so your normal sdkconfig and
# real-hardware builds are never touched.

param(
    [switch]$Clean,
    [string]$Target = "esp32"
)

$ErrorActionPreference = "Stop"

# Always run from the repository root (one level up from wokwi/).
Set-Location -LiteralPath (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))

if ($Clean) {
    Write-Host "Removing build_wokwi/ ..."
    Remove-Item -Recurse -Force "build_wokwi" -ErrorAction SilentlyContinue
}

# NOTE: SDKCONFIG must be passed as a CMake variable (-D), not an environment
# variable, otherwise ESP-IDF writes the Wokwi options into the root sdkconfig.
$common = @(
    "-B", "build_wokwi",
    "-DSDKCONFIG=build_wokwi/sdkconfig",
    "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.wokwi.defaults"
)

Write-Host "Target: $Target  (Wokwi simulation build)"
idf.py @common set-target $Target
idf.py @common build

Write-Host ""
Write-Host "Done. Firmware: build_wokwi/flasher_args.json"
Write-Host "Open this folder in VS Code and press F1 -> 'Wokwi: Start Simulator'."
