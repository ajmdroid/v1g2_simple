---
---
applyTo: "**"
---

Provide project context and coding guidelines that AI should follow when generating code, answering questions, or reviewing changes.

## Project Rails
- All work happens on `dev`. Do NOT create feature branches.
- Do not work in `main`.
- Do not push unless explicitly told.
- Commit after every change, with a clear message describing the change.
- Aways ensure /Users/ajmedford/v1g2_simple/scripts/ci-test.sh passes before committing.
- PERF_SLOS.md defines performance SLOs and scoring. Use it as a guide for performance-sensitive code.
- No deprecated JSON.
- Always review logs and gather evidence before making a change.
- Prefer a plan before making a change.
- Device AP Wi-Fi IP: `192.168.35.5`
- ./build.sh --clean -f -u      # Build everything, upload, no monitor
- ./build.sh -f -u              # Same without clean
- Use 'rg' in place of grep if available for faster searching: `rg <search-term>`

## Priority Order (highest → lowest)
All design and implementation decisions must preserve this priority stack:

1) V1 connectivity (must stay connected)
2) BLE ingest/drain (lowest-latency path; never block)
3) Display updates (responsive; never block BLE)
4) Audio alerts (best-effort; may drop, must not block)
5) Metrics collection (bounded time; degrade gracefully)
6) Wi-Fi / Web UI (off by default; maintenance mode)
7) Logging / persistence (best-effort; drops ok, corruption not; never block above)

## Decision Rule
If a feature risks increasing latency/jitter for any higher tier, it must be deferred, rate-limited, or made best-effort (drop/skip) rather than blocking.
