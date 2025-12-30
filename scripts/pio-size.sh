#!/usr/bin/env bash
set -euo pipefail

# Print the memory usage summary for the waveshare build environment.
pio run -e waveshare-349 -t size
