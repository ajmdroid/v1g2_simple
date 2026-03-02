# BLE + Display Real-Signal Soak Plan

Status: Draft for implementation
Date: 2026-02-22
Owner: firmware/runtime soak lane

## Goal

Exercise the real parser and real display pipeline with protocol-accurate alert traffic, not just HTTP preview toggles, so failures are actionable (`where`, `why`, `how to fix`).

This plan uses real ESP packet semantics from ValentineResearch `AndroidESPLibrary2` and maps directly to this firmware's hot path:

- BLE notify callback -> queue: `src/ble_connection.cpp`, `src/modules/ble/ble_queue_module.cpp`
- Packet parse: `src/packet_parser.cpp`
- Display/render path: `src/modules/display/display_orchestration_module.cpp`, `src/modules/display/display_pipeline_module.cpp`

## Protocol Alignment (Source-Checked)

- ESP framing: `0xAA ... 0xAB`
- Display packet ID: `0x31` (`INFDISPLAYDATA`)
- Alert row packet ID: `0x43` (`RESPALERTDATA`)
- Alert stream control IDs: start `0x41`, stop `0x42`
- BLE UUID path: V1 notify short `...B2CE`, long `...B4E0`

Source references:

- `AndroidESPLibrary2/BTUtil.java`
- `AndroidESPLibrary2/PacketId.java`
- `AndroidESPLibrary2/ResponseAlertData.java`
- `AndroidESPLibrary2/AlertData.java`
- `AndroidESPLibrary2/AlertDataProcessor.java`

## Test Architecture (Planned, No Code Yet)

1. Add runtime packet-injection mode in normal firmware (debug-gated, not `REPLAY_MODE`).
2. Feed injected packets through `BleQueueModule::onNotify(...)` so parser + display run exactly like live BLE.
3. Add a soak mode that drives packet scenarios while WiFi stress remains active.
4. Emit scenario frame index and packet window around first failure into soak artifacts.

## Scenario Fixture Format (Planned)

CSV or JSONL, one frame per record:

- `t_ms`: relative timestamp
- `char_uuid`: `0xB2CE` or `0xB4E0`
- `hex`: full ESP packet bytes
- `tag`: scenario step label

Example fields for alert row payload (`0x43`):

- byte0: high nibble `index`, low nibble `count`
- byte1..2: frequency
- byte3..4: front/rear strength raw
- byte5: band+direction bitmap (`0x1F` band field, `0xE0` direction field)
- byte6: aux0 (priority/junk/photo)

## Scenario Matrix

### A. Standard Radar

| ID | Pattern | Expected Parser/Display Behavior | Key Gates/Signals |
|---|---|---|---|
| `RAD-01` | Single K alert, stable front arrow, gradual strength rise/fall | One alert table publish cycle, stable priority, normal display redraw cadence | `parseFailures=0`, `alertTablePublishes>0`, `displayUpdates delta>min`, `loopMaxUs` |
| `RAD-02` | Single Ka alert with side->front arrow transitions | Priority arrow changes reflected without stale direction artifacts | `prioritySelectRowFlag`, `displayUpdates`, `dispPipeMaxUs` |
| `RAD-03` | X alert with intermittent mute bit | Mute state follows packet bits without sticky latch | `muted state transitions`, `displaySkips`, `bleDrainMaxUs` |

### B. Photo Radar

| ID | Pattern | Expected Parser/Display Behavior | Key Gates/Signals |
|---|---|---|---|
| `PHO-01` | K/Ka rows with `aux0 photoType=1..6` sweep | `hasPhotoAlert=true`, photo indicators/voice path reflect photo context | `hasPhotoAlert`, `displayUpdates`, `loopMaxUs` |
| `PHO-02` | Mixed table: one photo row + one normal row | Priority resolution stays deterministic, photo flag preserved in table | `prioritySelect*`, `alertTablePublishes`, `parseFailures=0` |

### C. Junk

| ID | Pattern | Expected Parser/Display Behavior | Key Gates/Signals |
|---|---|---|---|
| `JNK-01` | Row with `aux0 junk bit` only | `hasJunkAlert=true`; no parser instability | `hasJunkAlert`, `parseFailures=0` |
| `JNK-02` | Junk row replaced by valid row at same index | Duplicate replacement handled; published table reflects latest row | `alertTableRowReplacements`, `alertTablePublishes` |

### D. Laser

| ID | Pattern | Expected Parser/Display Behavior | Key Gates/Signals |
|---|---|---|---|
| `LAS-01` | Immediate laser event (single-row table) | Laser takes priority quickly, high-urgency display path stable | `priority index`, `displayUpdates`, `dispPipeMaxUs`, `loopMaxUs` |
| `LAS-02` | Laser burst on/off with rapid clear (`count=0`) | No stale lingering rows after clears; no assembly corruption | `alertTablePublishes`, `alertTableAssemblyTimeouts=0`, `parseFailures=0` |

### E. Multi-Bogey Priority Churn (Requested Focus)

| ID | Pattern | Expected Parser/Display Behavior | Key Gates/Signals |
|---|---|---|---|
| `MBP-01` | `count=5` weak K signals (multiple arrows), then faint Ka appears and grows over successive tables | Priority can transition from K to Ka as strength/context changes without table corruption | `prioritySelect*`, `displayUpdates`, `loopMaxUs`, `dispPipeMaxUs` |
| `MBP-02` | Immediate Ka appears at low strength (<4 bars equivalent) among K rows | Ka handling should be stable even when not initially dominant in strength | `priority transitions`, `bleDrainMaxUs`, `loopMaxUs` |
| `MBP-03` | K multi-bogey table preempted by immediate laser | Laser preemption should be immediate and clear, no stale K priority residue | `priority index`, `displayUpdates`, `alertTablePublishes` |

### F. Assembly Fault Cases

| ID | Pattern | Expected Parser/Display Behavior | Key Gates/Signals |
|---|---|---|---|
| `ASM-01` | Out-of-order rows (`2/3`, `1/3`, `3/3`) | Table publishes only when complete; row order normalized | `alertTablePublishes`, `parseFailures=0` |
| `ASM-02` | Missing row (never send index 2) until timeout | No false table publish; timeout counter increments once | `alertTableAssemblyTimeouts>0`, no crash |
| `ASM-03` | Duplicate same index with changed payload | New row replaces old row cleanly | `alertTableRowReplacements>0` |
| `ASM-04` | Count flip mid-stream (`count=3` -> `count=2`) | Old partial cache is not reused incorrectly | `alertTableAssemblyTimeouts`, `alertTablePublishes`, `parseFailures=0` |

### G. Transport Burst/Split

| ID | Pattern | Expected Parser/Display Behavior | Key Gates/Signals |
|---|---|---|---|
| `TRN-01` | Burst of back-to-back packets with minimal gap | Queue/backpressure may rise, but parser remains correct | `queueHighWater`, `queueDrops=0`, `parseFailures=0`, `bleDrainMaxUs` |
| `TRN-02` | Split packet delivery across notify events (framing across chunks) | Reassembly in RX buffer remains correct | `parseFailures=0`, `alertTablePublishes` |
| `TRN-03` | Mixed short+long characteristic delivery (`B2CE` + `B4E0`) | Packet routing does not break parser/display path | `parseFailures=0`, `displayUpdates` |

## Pass/Fail Policy (Execution)

Hard fail if any:

- `parseFailures delta > 0`
- `qDrop/queueDrops delta > 0`
- `oversizeDrops delta > 0`
- `loopMaxUs` above gate
- `bleDrainMaxUs` above gate
- `dispPipeMaxUs` above gate
- `alertTableAssemblyTimeouts` non-zero in scenarios not explicitly fault-injection

Advisory trend checks:

- `displaySkips` ratio
- `gpsObsDrops`

SLO basis: `docs/PERF_SLOS.md`

## Execution Ladder

1. `L1` Parser integrity: `RAD/PHO/JNK/LAS/ASM` scenarios without additional drive.
2. `L2` Display stress: same scenarios with display rendering and preview disabled.
3. `L3` Coexistence: add WiFi metrics polling during injection.
4. `L4` Endurance (`T100`): long run rotating `MBP + TRN + LAS` scenarios, stop on first hard failure and capture packet window.

## Artifacts Required Per Run

- `scenario.jsonl` (input stream)
- `metrics.jsonl` and `summary.md` (existing soak outputs)
- `first_failure_context.json` containing:
  - scenario ID
  - frame index
  - last 20 packets
  - gate that tripped first

## Immediate Next Implementation Tasks

1. Add debug API endpoints for scenario injection start/stop/status (debug build only).
2. Add fixture loader and scheduler to feed `BleQueueModule::onNotify(...)`.
3. Add soak runner mode `--drive-v1-scenarios <file>` and include scenario metadata in summary.
4. Implement first fixture set: `MBP-01`, `MBP-02`, `MBP-03`, `ASM-01..04`, `TRN-01..03`.
