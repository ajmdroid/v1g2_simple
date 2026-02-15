#!/usr/bin/env bash
set -euo pipefail

# Run PlatformIO static analysis for the waveshare environment.
# Preferred tool is configured via platformio_clangtidy.ini, but PlatformIO may
# fall back to cppcheck depending on host/toolchain availability.
pio check -e waveshare-349 --project-conf platformio_clangtidy.ini
