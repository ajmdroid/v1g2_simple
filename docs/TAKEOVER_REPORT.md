# V1-Simple: Senior Engineer Takeover Report

> **Date:** March 8, 2026 (updated — original draft March 7)
> **Codebase:** ESP32-S3 touchscreen display for Valentine One Gen2 radar detector
> **Version:** 4.0.0-dev (1,807 commits, 1,371 ahead of `main`)

---

## Executive Summary

This is a **well-engineered embedded project** that has been through a major
architecture overhaul (v3 → v4). The codebase is in good shape overall — the
modular rewrite is largely complete, test coverage is strong, and CI discipline
is above-average for an embedded project. The CI gate is more comprehensive than
initially assessed — it includes frontend linting, static analysis, frontend
unit tests with coverage, and 8 architectural contract scripts. The remaining
concerns are around frontend test depth, oversized page components, and
continued `main.cpp` decomposition.

**Release strategy:** `main` will be hard-reset to `dev` when v4.0.0 ships.
This is an intentional choice — no merge is planned, so the commit delta between
branches carries no merge-conflict risk.

---

## Codebase Stats

| Metric | Value |
|--------|-------|
| Firmware source (src/) | 44,659 lines across 204 files |
| Modules (src/modules/) | 16 directories |
| Frontend (interface/src/) | 8,493 lines (7,716 in Svelte) |
| Tests (test/) | 89 test files, 27,394 lines, 1,006 test cases, 3,712 assertions |
| Frontend tests | 3 files (colors, lockout utils, ToggleSetting component) |
| CI contract scripts | 8 scripts, 9 snapshot files |
| Web UI compressed size | 124 KB gzipped |
| data/ partition usage | 2.2 MB / 3.25 MB budget (68%) |
| Total commits | 1,807 |
| Recent velocity | 1,019 commits in Feb 2026, 153 in Mar 2026 (to date) |

---

## What's Good

### Architecture (A-)
- **Clean modular decomposition**: 16 modules under `src/modules/` with
  three DI patterns (direct pointer, callback, providers). Composition-root
  pattern in `main.cpp`.
- **Thin main loop**: Loop phases decomposed into individual modules
  (`LoopIngestModule`, `LoopDisplayModule`, etc.) with typed `Context`/`Result`
  structs for data flow.
- **Clear priority stack**: BLE > Display > Audio > WiFi > Logging — enforced
  architecturally and verified by contract scripts.

### Testing (A)
- 89 test files, 27,394 lines using Unity framework.
- 1,006 test cases, 3,712 assertions with edge-case coverage (packet corruption,
  millis wraparound, GPS boundary conditions).
- All critical subsystems have dedicated test suites: BLE (4), lockout (7+),
  WiFi (16+), GPS (4), display (3).
- Device-level integration tests for heap stress, coexistence, boot sequence.
- Bench ladder qualification (L0–L8) with automated failure classification.

### CI & Contract Enforcement (A)
- 8 contract scripts checking architectural invariants (BLE hot-path discipline,
  SD mutex safety, display flush stability, main loop ordering, WiFi API
  contracts, perf CSV schema, frontend HTTP resilience, extern/global usage).
- 9 contract snapshot files — modifications require explicit `--update`.
- Pre-push gate `ci-test.sh` runs: contracts → native tests → frontend lint
  (`svelte-check`) → frontend unit tests with coverage → web build → web
  deploy → asset budget check → firmware static analysis (`pio check`) →
  firmware build → size report.
- Ship gate `iron-gate.sh` adds hardware integration tests.

### Code Quality (A-)
- **Zero TODO/FIXME/HACK/WORKAROUND markers** across 44,659 lines.
- Consistent module naming (`*Module`, `*Service`, `*Store`).
- 47 extern globals (tracked by extern usage contract script).
- Memory safety well-handled: RAII for BLE, proper cleanup for display,
  bounds checking on file reads.
- Excellent developer docs: `DEVELOPER.md` has critical rules (no BLE client
  deletion, single-threaded display, battery latch timing).

### Documentation (A-)
- `ARCHITECTURE.md` — accurate module map with responsibilities table.
- `PERF_SLOS.md` — quantitative SLOs with scoring tooling.
- `TESTING.md` — canonical test workflows with hardware validation procedures.
- `TROUBLESHOOTING.md` — user-facing issue resolution guide.
- `API.md` — complete REST API reference.

### Frontend (B+)
- SvelteKit 5 + TailwindCSS + DaisyUI — modern, appropriate for embedded SPA.
- 124 KB gzipped total — excellent for ESP32's LittleFS.
- Zero XSS vectors (`@html` usage: none), HTTP resilience enforced by contract.
- Default password warning banner with session-aware dismissal.
- 3 unit tests with Vitest + testing-library; coverage infra wired into CI.

---

## What's Concerning

### 1. Frontend test coverage is minimal (MEDIUM)

Only 3 test files exist for 8,493 lines of frontend code (colors.test.js,
lockout.test.js, ToggleSetting.test.js). Coverage infrastructure is fully wired
into CI (`vitest --coverage`, testing-library deps installed), but actual test
count is very low. Existing tests cover utility functions and one component —
no page-level behavioral tests.

### 2. Frontend page components are oversized (MEDIUM)

- `lockouts/+page.svelte` — **1,118 lines** (down from 1,910 — partially decomposed)
- `settings/+page.svelte` — **835 lines**
- `profiles/+page.svelte` — **711 lines**
- `cameras/+page.svelte` — **648 lines**

Some extraction has happened (LockoutZonesCard, LockoutLearningRulesCard,
LockoutZoneEditorModal), but page files still mix API calls, state management,
and rendering logic.

### 3. `dev-refactor` branch concern is resolved (INFO)

The `dev-refactor` branch no longer exists. This concern is closed and removed
from active takeover risk tracking.

### 4. `main.cpp` still at 1,420 lines (LOW-MEDIUM)

Down from 3,100 pre-refactor. Loop phase modules are extracted, but setup and
initialization logic remains large. Further decomposition into a boot module
would reduce bus factor risk.

### 5. Largest service files (LOW)

- `debug_api_service.cpp` — 1,927 lines (many endpoints, inherently verbose)
- `gps_api_service.cpp` — 991 lines
- `lockout_api_service.cpp` — 922 lines
- `wifi_manager.cpp` — 826 lines

Service files tend to be verbose by nature. Not urgent.

---

## Finding Closure Criteria (No-Breakage)

| Finding | Closure Criteria |
|---|---|
| Frontend test depth | Route-level tests exist for `lockouts`, `settings`, `profiles`, `cameras`, and pass in CI. |
| Oversized page components | Page decomposition completed so each targeted route page is no longer monolithic and stays within the current remediation targets. |
| `main.cpp` size | `src/main.cpp` reduced to `<=1100` lines with setup/loop behavior unchanged. |
| Large service files | Debug/GPS/Lockout/WiFi service regrouping completed with API routes/signatures unchanged and wrapper tests still passing. |

Authoritative acceptance sources:

- [`docs/TESTING.md`](TESTING.md) for cycle workflow, bench ladder policy, and failure classification.
- [`docs/PERF_SLOS.md`](PERF_SLOS.md) for hard SLO pass/fail thresholds and scoring expectations.

---

## Operational Qualification Baseline (Authoritative)

1. **Source of truth**
   - Use `docs/TESTING.md` for workflow and ladder policy.
   - Use `docs/PERF_SLOS.md` for hard SLO pass/fail thresholds.

2. **Preconditions before any stability/perf cycle**
   - V1 must be powered on and connected before starting runs.
   - Bench truth profile is fixed to AP + display drive + transition flaps.
   - Keep physical setup consistent (power source, placement, AP client behavior).

3. **Canonical cycle commands**
   - On firmware/runtime code changes:
     - `./build.sh --clean --upload --upload-fs`
     - `./scripts/device-test.sh --duration-seconds 240 --rad-duration-scale-pct 200`
     - `./scripts/iron-gate.sh --skip-flash`
   - If code changes during testing, restart cycle from the first command.

4. **Bench stress ladder acceptance bar**
   - Ladder levels L0-L8 are fixed and executed in order per `TESTING.md`.
   - Each level requires `2/2` pass.
   - Minimum acceptable release bar: **L6 cleared 2/2**.

5. **Failure policy and release decision**
   - Class A (reliability-critical) before or at L6 is **not acceptable**.
   - First sustained Class B at L7+ (with no Class A before L6) is acceptable with optimization backlog.
   - Class C transients require rerun and reclassification rules exactly as documented in `TESTING.md`.

6. **Evidence and traceability**
   - Latest validated cycle pass artifact (March 9, 2026):
     - `/Users/ajmedford/v1g2_simple/.artifacts/test_reports/device_test_20260309_062011/summary.md`
   - Every qualification campaign must record highest cleared level, first failing level, failure class, and artifact paths.

---

## First-Week Plan

| Priority | Task | Why |
|----------|------|-----|
| **P0** | Run full CI gate (`ci-test.sh`) and fix any failures | Establish green baseline before touching anything |
| **P0** | Run `iron-gate.sh` with hardware (if available) | Verify device stability under current code |
| **P1** | Clarify `dev-refactor` branch status — merge or delete | Only unresolved branch; reduces confusion |
| **P1** | Expand frontend test coverage — target page-level tests for lockouts and settings | 3 tests for 8,493 lines is insufficient; infra is already in place |
| **P2** | Continue splitting oversized page components (lockouts 1,118, settings 835) | Maintainability; good progress already made |
| **P2** | Investigate extracting `main.cpp` setup/init into a boot module | 1,420 lines; reduce bus factor risk for boot understanding |
| **P3** | Review audio file inventory — identify any unused `.mul` files | 123 files at 1.9 MB is the dominant data/ consumer |

---

## Overall Grade: B+/A-

This is a strong codebase for an embedded hobby/prosumer project. The
architecture is sound, testing is thorough (1,006 cases, 3,712 assertions), and
the CI gate is comprehensive — contracts, linting, static analysis, unit tests
with coverage, asset budgets, and firmware builds all run in the pre-push gate.
The module migration from v3 to v4 was executed well — the code reads cleanly
and the priority stack is respected throughout.

The release strategy (hard-reset `main` to `dev` at ship time) eliminates
merge-conflict risk. The remaining work is incremental: frontend test depth,
component decomposition, and continued `main.cpp` extraction.
