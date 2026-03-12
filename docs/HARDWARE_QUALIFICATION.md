# Hardware Qualification

> Status: active
> Last validated against scripts: March 12, 2026

This file defines the trusted single-board hardware gate.

## Authoritative Command

```bash
./scripts/qualify_hardware.sh
```

## Fixed Sequence

`qualify_hardware.sh` runs this sequence without requiring manual tuning:

1. Build the real firmware image and LittleFS image.
2. Flash the real firmware image and filesystem image to the connected board.
3. Verify the device serial port is available.
4. Verify the metrics endpoint is reachable.
5. Run `./scripts/run_device_tests.sh --full`.
6. Run a `300s` real-firmware telemetry soak with metrics required and `--profile drive_wifi_ap`.
7. Run a `300s` display-preview telemetry soak with metrics required and forced preview driving.
8. Fail qualification on any non-zero result, including `INCONCLUSIVE`.

Artifacts are written to:

```bash
.artifacts/test_reports/qualification_<timestamp>/
```

## Prerequisites

- ESP32-S3 connected over USB.
- Setup AP enabled so the metrics endpoint is reachable.
- Metrics URL available at `http://192.168.35.5/api/debug/metrics`, or supplied via `--metrics-url`.

If the metrics precheck fails, qualification stops immediately instead of reporting a false pass.

## Non-Authoritative Hardware Tools

These scripts still exist for debugging and exploration, but they are not the release gate by themselves:

- `./scripts/device-test.sh` — manual hardware triage
- `./scripts/run_real_fw_soak.sh`
