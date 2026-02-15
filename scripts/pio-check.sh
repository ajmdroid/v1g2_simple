#!/usr/bin/env bash
set -euo pipefail

# Run PlatformIO static analysis for the waveshare environment.
pio check -e waveshare-349
