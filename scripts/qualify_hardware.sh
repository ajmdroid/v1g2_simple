#!/usr/bin/env bash
#
# DEPRECATED: qualify_hardware.sh has been superseded by ./scripts/hardware/test.sh
#
# This script now redirects to the unified hardware test entry point.
# Use:
#   ./scripts/hardware/test.sh --all --board-id release --strict
#
set -euo pipefail

echo "================================================================"
echo "DEPRECATED: qualify_hardware.sh is retired."
echo "Redirecting to: ./scripts/hardware/test.sh --all --strict"
echo "================================================================"
echo ""

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Forward --board-id if provided, otherwise default to release
ARGS=(--all --strict)
while [[ $# -gt 0 ]]; do
  case "$1" in
    --board-id)
      ARGS+=(--board-id "$2")
      shift 2
      ;;
    *)
      shift
      ;;
  esac
done

# Ensure --board-id is present (default: release)
if [[ ! " ${ARGS[*]} " =~ " --board-id " ]]; then
  ARGS+=(--board-id release)
fi

exec "$ROOT_DIR/scripts/hardware/test.sh" "${ARGS[@]}"
