#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export HARDWARE_TEST_SELF_NAME="./test.sh"
exec "$ROOT_DIR/scripts/hardware/test.sh" "$@"
