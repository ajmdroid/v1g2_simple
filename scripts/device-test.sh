#!/usr/bin/env bash
#
# DEPRECATED: device-test.sh has been superseded by ./scripts/hardware/test.sh
#
# The unified hardware test entry point includes RAD scenario validation,
# uptime continuity checking, device tests, and soak steps with structured
# artifact storage and run-to-run comparison.
#
# Use:
#   ./scripts/hardware/test.sh --all --board-id release
#
echo "⚠️ Deprecated: use ./scripts/hardware/test.sh" >&2
echo "================================================================" >&2
echo "DEPRECATED: device-test.sh is retired." >&2
echo "Redirecting to: ./scripts/hardware/test.sh --all" >&2
echo "================================================================" >&2
echo "" >&2

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT_DIR/scripts/hardware/test.sh" --all
