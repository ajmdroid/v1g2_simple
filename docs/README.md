# Documentation Map

This file is the documentation index for the repo. It is not itself a testing
or release policy document.

## Authoritative

- `TESTING.md`: current code-gate and evidence model.
- `HARDWARE_QUALIFICATION.md`: current trusted single-board hardware qualification path.
- `PERF_SLOS.md`: current perf thresholds and scoring rules.

## Reference

- `MANUAL.md`: comprehensive technical reference — architecture, BLE protocol,
  display, audio, storage, troubleshooting, developer guide, and API quick reference.
- `API.md`: full HTTP REST API reference with request/response schemas.
- `ROAD_MAP_FORMAT.md`: GPS road-map binary format specification.
- `WINDOWS_SETUP.md`: Windows-specific setup and troubleshooting.

## Rules

- Release/testing authority comes from the Authoritative set only.
- Reference docs may lag implementation details and must not be treated as policy.
- Each topic has ONE home. Architecture and troubleshooting live in MANUAL.md.
- Historical material should be recovered from git history, not the active docs tree.
