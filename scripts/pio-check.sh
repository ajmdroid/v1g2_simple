#!/usr/bin/env bash
set -euo pipefail

# Run PlatformIO's static analysis + build-time checks for the waveshare environment.
# Run PlatformIO's clang-tidy check via the override config to avoid cppcheck issues.
pio check -e waveshare-349 --project-conf platformio_clangtidy.ini
