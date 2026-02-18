# Lockout Parity Plan (JBV1 / V1 Companion Gap Closure)

## Objective
Close the highest-impact lockout-system gaps with a low-risk, staged rollout that preserves runtime priorities:

1. Direction-aware matching and learning policy.
2. Manual lockout management APIs (create/edit/delete/list).
3. Import/export and operator tooling.
4. Web UI modernization for safer, faster lockout workflows.

## Baseline (Current Firmware)
- Matching dimensions: `lat/lon + band + frequency (+ tolerance)`.
- Runtime modes: `off`, `shadow`, `advisory`, `enforce`.
- Learner supports promotion/unlearn policies and pending candidate persistence.
- Zone management API supports list + learned-zone delete only.
- Lockouts web UI provides tuning + observability, but manual operations are limited.

## Guardrails
- Never block BLE ingest/drain or V1 connectivity paths.
- New lockout operations stay bounded and rate-limited.
- Backward compatibility for existing `/v1simple_lockout_zones.json`.
- No deprecated JSON fields in newly added APIs.

## Milestone 1: Direction-Aware Runtime (Schema + Matching)
### Scope
- Extend lockout entry model with optional direction policy:
  - `directionMode`: `all`, `forward`, `reverse`.
  - `headingDeg` and heading tolerance metadata for learned/manual entries.
- Update enforcer matching to apply direction gates when course is valid.
- Preserve legacy behavior when direction metadata is absent.

### Firmware Deliverables
- `LockoutEntry`, `LockoutIndex`, `LockoutStore` updates.
- Direction-aware evaluate path in enforcer/index.
- Settings/API exposure for default direction behavior on learned entries.
- Tests:
  - Legacy entries still match as before.
  - Direction mismatch blocks mute.
  - Direction match allows mute.
  - No-course fallback behavior remains safe.

## Milestone 2: Manual CRUD + Import/Export APIs
### Scope
- Add manual lockout endpoints:
  - `POST /api/lockouts/zones/create`
  - `POST /api/lockouts/zones/update`
  - `POST /api/lockouts/zones/delete` (extended to manual + learned)
  - `GET /api/lockouts/zones/export`
  - `POST /api/lockouts/zones/import`
- Support explicit manual flags and direction metadata in payloads.
- Validate and clamp all mutable numeric fields on ingest.

### Firmware Deliverables
- Lockout API service route handlers and validation helpers.
- Atomic import flow with rollback on invalid payload.
- Dirty-mark + persistence integration after mutations.
- Tests:
  - Create/update/delete success and validation failures.
  - Import rejects malformed payloads.
  - Export round-trip consistency.

## Milestone 3: Web UI Modernization (Lockouts)
### UX Goals
- Reduce clunky workflows and reduce operator error risk.
- Make lockout operations explicit, auditable, and reversible.

### UI Scope
- Replace single-table workflow with tabbed sections:
  - `Runtime`, `Active Zones`, `Pending`, `Import/Export`, `Audit`.
- Add manual zone editor drawer:
  - point/radius, band/frequency, direction policy, source tags.
- Add staged-change review panel before destructive operations.
- Add bulk actions:
  - select/delete/export selected.
- Add import wizard:
  - schema validation summary, dry-run preview, apply action.
- Improve map affordances:
  - open map links for points and candidate clusters.

### UI Safety Patterns
- Distinct warning states for Ka and enforce-mode mutations.
- Confirm dialogs for deletes/import apply.
- Form-level validation with inline clamping hints and server error mapping.
- Explicit "dirty vs synced" status per section.

## Rollout Sequence
1. Land schema and matching first with backward compatibility.
2. Add CRUD/import/export APIs and test coverage.
3. Ship UI refresh against stable API contracts.
4. Run drive-test checklist and compare behavior traces before/after.

## Verification Matrix
- Unit tests: lockout entry/index/store/enforcer/api and UI fetch/save flows.
- Integration checks:
  - lockout match/miss with heading changes.
  - import/export round-trip on real persisted files.
  - no measurable regression in BLE drain timing.
- Safety checks:
  - enforce guard still blocks when core-drop thresholds trip.
  - pre-quiet and mute path unchanged for legacy zones.

