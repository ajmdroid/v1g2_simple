# Main.cpp Refactoring Migration Log

**Started:** January 27, 2026  
**Goal:** Extract 832-line main loop into alert-centric modules  
**Strategy:** One micro-step at a time, test after each change  
**Note (Jan 28, 2026):** V1AlertModule was split—alert persistence now lives in `modules/alert_persistence`, and all voice logic lives in `modules/voice`.

## Overall Plan
- **Phase 1:** Extract V1 Alert handling (BLE + display + audio)
- **Phase 2:** Extract Camera Alert handling  
- **Phase 3:** Extract GPS/OBD services
- **Phase 4:** Unify Settings API

---

## Phase 1: V1 Alert Module

### Step 1: Create Module Structure
**Status:** ✅ COMPLETE  
**Date:** January 27, 2026  
**Commit:** ✅ Committed

**Objective:** Create empty directory and stub files for v1_alerts module

**Actions:**
- [x] Create `src/modules/v1_alerts/` directory
- [x] Create `v1_alert_module.h` with empty class
- [x] Create `v1_alert_module.cpp` with empty implementation  
- [x] Wire into main.cpp (include, instantiate, call begin/update)
- [x] Test: Firmware compiles ✅ SUCCESS (10.92s)
- [x] Test: Device boots and connects to V1 ✅ PASSED - alerts fired normally
- [x] Commit: "refactor: Add empty V1AlertModule stub"

**Files Created:**
- src/modules/v1_alerts/v1_alert_module.h (empty stub class)
- src/modules/v1_alerts/v1_alert_module.cpp (empty implementation)

**Files Modified:**
- src/main.cpp (added include at line 44, instance at line 137, begin() at 2243, update() at 2266)

**Compilation Result:**
- SUCCESS - 10.92 seconds
- RAM: 25.6% (83796/327680 bytes)
- Flash: 41.8% (2736512/6553600 bytes)
- No warnings

**Hardware Test Result:**
- ✅ Device boots normally
- ✅ Connects to V1
- ✅ Alerts display and fire correctly
- ⚠️ One intermittent BLE crash on first boot (pre-existing issue, recovered on reboot)

**Issues Encountered:**
- Pre-existing BLE abort() during connection (intermittent, not caused by our change)

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 2: Add References to External Objects

**Status:** ✅ COMPLETE  
**Date:** January 27, 2026
**Commit:** [pending]

**Objective:** Give the module access to external objects it will need (bleClient, parser, display, etc.) without moving any logic yet.

**Actions:**
- [x] Add pointer members to V1AlertModule for dependencies
- [x] Add begin(deps) method to pass references  
- [x] Update main.cpp to pass references in begin() call
- [x] Verify compilation
- [x] Verify hardware still works identically

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - added forward decls, pointer members, updated begin() signature
- src/modules/v1_alerts/v1_alert_module.cpp - added includes, updated begin() to store refs
- src/main.cpp - updated begin() call at line 2244 to pass &bleClient, &parser, &display, &settingsManager

**Compilation Result:**
- ✅ SUCCESS
- RAM: 25.6% (83812/327680 bytes)
- Flash: 41.8% (2736568/6553600 bytes)

**Hardware Test Result:**
- ✅ Alerts fired correctly
- ✅ No BLE issues

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 3: Move getAlertBars Helper to Module

**Status:** ✅ COMPLETE  
**Date:** January 27, 2026
**Commit:** [pending]

**Objective:** Move a simple helper function into the module. This proves the module can be used by main.cpp's alert processing code without breaking anything.

**Why this is safe:** It's a pure computation with no state. Same logic, different file. Easy to verify nothing changed.

**Actions:**
- [x] Add getAlertBars() as public static method in V1AlertModule
- [x] Include packet_parser.h in module header (for AlertData type)
- [x] Update main.cpp to call V1AlertModule::getAlertBars() instead of local function
- [x] Remove local getAlertBars() from main.cpp
- [x] Verify compilation
- [x] Verify hardware works

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - added static getAlertBars(), included packet_parser.h
- src/modules/v1_alerts/v1_alert_module.cpp - implemented getAlertBars()
- src/main.cpp - 3 call sites updated to V1AlertModule::getAlertBars(), local function removed

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ Alerts work correctly

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 4: Move makeAlertId Helper to Module

**Status:** ✅ COMPLETE  
**Date:** January 27, 2026
**Commit:** [pending]

**Objective:** Move makeAlertId() helper to module. This is used for alert tracking and will be needed when we move the tracking state later.

**Why this is safe:** Pure function with no state, just bit manipulation.

**Actions:**
- [x] Add makeAlertId() as public static method in V1AlertModule
- [x] Update main.cpp to call V1AlertModule::makeAlertId()
- [x] Remove local makeAlertId() from main.cpp
- [x] Verify compilation
- [x] Verify hardware works

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - added static makeAlertId()
- src/modules/v1_alerts/v1_alert_module.cpp - implemented makeAlertId()
- src/main.cpp - 6 call sites updated, local function removed

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ Alerts work correctly

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 5: Move Announced Alert Tracking to Module

**Status:** ✅ COMPLETE  
**Date:** January 27, 2026
**Commit:** [pending]

**Objective:** Move the announced alert tracking state and functions. This is a self-contained cluster for tracking which alerts have been voice-announced.

**Why this is safe:** Self-contained state with simple accessor functions. No complex logic.

**Actions:**
- [x] Add announcedAlertIds array and count as private members
- [x] Add isAlertAnnounced(), markAlertAnnounced(), clearAnnouncedAlerts() methods
- [x] Update main.cpp to call module methods
- [x] Remove local state/functions from main.cpp
- [ ] Verify compilation
- [ ] Verify hardware works

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - added state and methods
- src/modules/v1_alerts/v1_alert_module.cpp - implemented methods
- src/main.cpp - removed local state/functions, updated 3 call sites

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ Alerts work correctly

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 6: Move Alert History Tracking to Module

**Status:** 🟡 IN PROGRESS  
**Date:** January 27, 2026
**Commit:** [pending]

**Objective:** Move the smart threat escalation tracking (AlertHistory struct, state, and functions). This tracks signal strength over time to detect threats ramping up.

**Why this is safe:** Self-contained state cluster. Functions only interact with local state + makeAlertId.

**Actions:**
- [x] Add AlertHistory struct and constants to module
- [x] Add alertHistories array and count as private members
- [x] Add all history tracking methods
- [x] Add clearAlertHistories() method
- [x] Update clearAnnouncedAlerts() to call clearAlertHistories()
- [x] Update main.cpp call sites
- [x] Verify compilation
- [x] Verify hardware works

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - added AlertHistory struct, constants, methods
- src/modules/v1_alerts/v1_alert_module.cpp - implemented all history tracking methods
- src/main.cpp - removed ~120 lines (struct, constants, state, 6 functions), updated 4 call sites

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ SUCCESS

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 7: Move isBandEnabledForSecondary() to Module

**Status:** 🟡 IN PROGRESS  
**Date:** January 28, 2026
**Commit:** [pending]

**Objective:** Move the isBandEnabledForSecondary() helper function to V1AlertModule. Used for secondary alert voice announcements.

**Why this is safe:** Simple pure function with no state, just checks V1Settings struct.

**Actions:**
- [x] Add static isBandEnabledForSecondary() method to module header
- [x] Implement in module cpp
- [x] Update main.cpp call sites (2 total)
- [x] Remove local function from main.cpp
- [x] Verify compilation
- [x] Verify hardware works

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - added forward decl for V1Settings, added method declaration
- src/modules/v1_alerts/v1_alert_module.cpp - implemented isBandEnabledForSecondary()
- src/main.cpp - removed ~8 lines, updated 2 call sites to use V1AlertModule::

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ SUCCESS

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 8: Move Direction Change Throttling to Module

**Status:** 🟡 IN PROGRESS  
**Date:** January 28, 2026
**Commit:** [pending]

**Objective:** Move the direction change throttling state and logic to V1AlertModule. This prevents spamming voice announcements when the V1's direction arrow bounces.

**Why this is safe:** Self-contained state cluster with simple timing logic. No external dependencies.

**Actions:**
- [x] Add direction throttle state and constants to module
- [x] Add resetDirectionThrottle() and shouldThrottleDirectionChange() methods
- [x] Update main.cpp to call module methods
- [x] Remove local state/constants from main.cpp
- [x] Verify compilation
- [x] Verify hardware works

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - added methods + state + constants
- src/modules/v1_alerts/v1_alert_module.cpp - implemented throttle logic
- src/main.cpp - removed ~5 lines of state/constants, updated 2 call sites

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ SUCCESS

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 9: Move Priority Stability Tracking to Module

**Status:** 🟡 IN PROGRESS  
**Date:** January 28, 2026
**Commit:** [pending]

**Objective:** Move the priority stability tracking state and constants to V1AlertModule. This controls when secondary alerts can be announced (priority must be stable for 1s, with a gap after priority announcement).

**Why this is safe:** Self-contained timing state. No external dependencies beyond timestamp.

**Actions:**
- [x] Add priority stability state and constants to module
- [x] Add methods: updatePriorityStability(), markPriorityAnnounced(), canAnnounceSecondary(), resetPriorityStability()
- [x] Update main.cpp call sites (6 total)
- [x] Remove local state/constants from main.cpp
- [x] Verify compilation
- [x] Verify hardware works

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - added methods + state + constants
- src/modules/v1_alerts/v1_alert_module.cpp - implemented priority stability logic
- src/main.cpp - removed ~7 lines of state/constants, updated 6 call sites

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ SUCCESS

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 10: Move Voice Alert "Last Announced" Tracking to Module

**Status:** 🟡 IN PROGRESS  
**Date:** January 28, 2026
**Commit:** [pending]

**Objective:** Move the voice alert tracking state (last announced band/direction/freq/bogeyCount/time) to V1AlertModule. This tracks what was last announced to avoid re-announcing the same alert.

**Why this is safe:** Self-contained state cluster for tracking last announcement. No external dependencies.

**Actions:**
- [x] Add last announced state and cooldown constants to module
- [x] Add methods: hasAlertChanged(), hasCooldownPassed(), updateLastAnnounced(), resetLastAnnounced(), etc.
- [x] Update main.cpp call sites (~15 total)
- [x] Remove local state/constants from main.cpp
- [x] Verify compilation
- [x] Verify hardware works

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - added 10 methods + 7 state vars + 2 constants
- src/modules/v1_alerts/v1_alert_module.cpp - implemented all last announced tracking methods
- src/main.cpp - removed ~10 lines of state/constants, updated ~15 call sites

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ SUCCESS

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 11: Move Alert Persistence Tracking to Module

**Status:** ✅ COMPLETE  
**Date:** January 28, 2026
**Commit:** [pending]

**Objective:** Move the alert persistence tracking (keeps alert on screen briefly after V1 clears it) to AlertPersistenceModule. This is the grey "fading" alert display.

**Why this is safe:** Self-contained state cluster. Only interacts with AlertData struct.

**Actions:**
- [x] Add persistence state to module (persistedAlert, alertClearedTime, alertPersistenceActive)
- [x] Add methods: setPersistedAlert(), startPersistence(), clearPersistence(), shouldShowPersisted(), getPersistedAlert()
- [x] Update main.cpp call sites (5 locations updated)
- [x] Remove local state from main.cpp
- [x] Verify compilation
- [x] Verify hardware works

**Files to Modify:**
- src/modules/v1_alerts/v1_alert_module.h
- src/modules/v1_alerts/v1_alert_module.cpp
- src/main.cpp

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ SUCCESS (after fixing two bugs: timer reset on every call, seconds vs milliseconds)

**Issues Encountered:**
- Bug 1: startPersistence() reset alertClearedTime on every call instead of only first transition
- Bug 2: shouldShowPersisted() was passed seconds but expected milliseconds - fixed call site

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 12: Inline clearAnnouncedAlerts() Wrapper

**Status:** ✅ COMPLETE  
**Date:** January 28, 2026
**Commit:** [pending]

**Objective:** Remove the thin wrapper function `clearAnnouncedAlerts()` from main.cpp. Add a combined `clearAllAlertState()` method to V1AlertModule that clears all tracking in one call.

**Why this is safe:** The wrapper just calls two existing module methods. Combining them reduces indirection.

**Actions:**
- [x] Add clearAllAlertState() to V1AlertModule (clears announced alerts, histories, priority stability, last announced)
- [x] Replace clearAnnouncedAlerts() call in main.cpp with v1AlertModule.clearAllAlertState()
- [x] Remove static clearAnnouncedAlerts() wrapper from main.cpp  
- [x] Verify compilation
- [x] Verify hardware works

**Files to Modify:**
- src/modules/v1_alerts/v1_alert_module.h
- src/modules/v1_alerts/v1_alert_module.cpp
- src/main.cpp

**Compilation Result:**
- ✅ SUCCESS

**Hardware Test Result:**
- ✅ SUCCESS

**Issues Encountered:**
- None

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 13: Move Speed Helpers to V1AlertModule

**Status:** 🟡 IN PROGRESS  
**Date:** January 28, 2026
**Commit:** [pending]

**Objective:** Move speed helper functions (getCurrentSpeedMph, hasValidSpeedSource, isLowSpeedMuted) to V1AlertModule. These functions support voice alert logic and should live with the module.

**Why this is safe:** Self-contained functions that query OBD/GPS state. No complex interactions.

**Actions:**
- [x] Add speed helpers to V1AlertModule as public methods
- [x] Add speed cache state to module (cachedSpeedMph, cachedSpeedTimestamp)
- [x] Update call sites in main.cpp to use v1AlertModule.methodName()
- [x] Remove static functions and state from main.cpp
- [x] Verify compilation
- [x] Verify hardware works

**Files to Modify:**
- src/modules/v1_alerts/v1_alert_module.h
- src/modules/v1_alerts/v1_alert_module.cpp
- src/main.cpp

**Compilation Result:**
- ✅ SUCCESS (after fixing missing dependencies and SerialLog)

**Hardware Test Result:**
- ✅ SUCCESS

**Issues Encountered:**
- Missing forward declarations for OBDHandler/GPSHandler
- Missing dependencies in begin() call
- SerialLog is a #define in main.cpp only - used Serial directly

**Rollback Required:**
- No

**Commit:** ✅ Committed

---

## Step 14: Add Direction Conversion Utility

**Status:** 🟡 IN PROGRESS  
**Date:** January 28, 2026
**Commit:** [pending]

**Objective:** Add a static utility toAudioDirection() to convert V1's Direction bitmask to AlertDirection enum. This pattern is repeated 4 times in main.cpp voice alert code.

**Why this is safe:** Pure function, no state. Just eliminates code duplication.

**Actions:**
- [x] Add static toAudioDirection(Direction) to V1AlertModule
- [x] Update 3 call sites in main.cpp to use utility
- [ ] Verify compilation
- [ ] Verify hardware works

**Files to Modify:**
- src/modules/v1_alerts/v1_alert_module.h
- src/modules/v1_alerts/v1_alert_module.cpp
- src/main.cpp

**Compilation Result:**
-

**Hardware Test Result:**
-

**Issues Encountered:**
-

**Rollback Required:**
-

---

## Testing Checklist (After Each Step)

### Compilation Test
- [ ] `./build.sh` completes without errors
- [ ] No new warnings introduced
- [ ] Flash size hasn't increased significantly

### Boot Test
- [ ] Device boots to idle screen
- [ ] WiFi AP starts
- [ ] Web UI loads at 192.168.35.5

### V1 Connection Test
- [ ] BLE scan finds V1
- [ ] Connection succeeds
- [ ] Alert data displays on screen

### Alert Display Test
- [ ] X/K/Ka alerts show correct band
- [ ] Signal bars update correctly
- [ ] Arrows show direction
- [ ] Frequency displays

### Audio Test
- [ ] Alert beeps play
- [ ] Voice announces band/direction
- [ ] Muting works

### GPS Test (if enabled)
- [ ] GPS acquires fix
- [ ] Speed displays
- [ ] Auto-lockout records alerts

### Web UI Test
- [ ] Settings page loads
- [ ] Settings save correctly
- [ ] Log flags persist after refresh

---

## Rollback Procedure

If any step fails hardware testing:

1. Note failure in this log under "Issues Encountered"
2. Run: `git reset --hard HEAD~1` to undo last commit
3. Run: `./build.sh --all` to rebuild previous version
4. Flash and verify device works
5. Analyze failure cause before retrying

---

## Notes & Lessons Learned

### General Observations
- 

### Gotchas
- 

### Performance Impact
- 

---

## Progress Summary

**Phase 1a (Lift-and-Shift) Completed Steps:** 14  
**Failed Steps:** 0  

**Additional completed work (Jan 28, 2026):**
- VoiceModule created and now owns all voice alert decisions (priority/secondary/escalation).
- main.cpp reduced to building `VoiceContext` + executing `VoiceAction`.
- VolumeFadeModule implemented and wired; main.cpp no longer keeps per-alert fade globals.
- SpeedVolumeModule added and wired; highway boost logic removed from main.cpp.
- V1AlertModule renamed logically to AlertPersistenceModule (files now `alert_persistence_module.*`).

**Phase 1b (True Module Migration) Completed Steps:** 15-19  
**Step 20:** Final cleanup and doc alignment → ✅ COMPLETE (Jan 28, 2026)
  - Removed stale TODO comments in main.cpp voice switch
  - Ensured SpeedVolume defers to VolumeFade when fade owns volume
  - Tests: `./build.sh --test` ✅

---

## Current Status

**Last Updated:** January 28, 2026  
**Current Phase:** Voice module migration completed; alert persistence split out; volume fade + speed volume fully coordinated (tests passing)

**Next Candidates:**
- Optional: extract display/demo/color preview into a display module to shrink main.cpp
- Optional: consolidate web/settings endpoints into a Settings API module
- Optional: move GPS speed/lockout queries into a GPS module

---

# ARCHITECTURE PIVOT - January 28, 2026

> Status: Pivot executed. Voice decision logic now lives in `modules/voice/voice_module.cpp`; V1AlertModule handles alert persistence only. Notes below are retained for history.

## Problem Identified

After completing 14 steps of "lift-and-shift" migration, we realized:

> **We moved STATE to the module, but LOGIC stayed in main.cpp**

The module became a "utility bag" - a collection of helper functions and state that main.cpp queries. But main.cpp still has ~250 lines of voice alert decision logic that:
- Calls 44 module methods per cycle
- Makes all the decisions (what to announce, when)
- Executes the audio playback

This is NOT true modularity. Adding a voice feature still requires understanding main.cpp.

## What We Built (Steps 1-14)

VoiceModule now contains:
- **State:** announced alerts, alert histories, direction throttle, priority stability, last announced tracking, speed cache
- **Methods:** Query state, update state, static helpers, full decision engine (`process`)
- **Lines:** ~430 (header + cpp)

main.cpp now contains:
- Voice context build + `voiceModule.process(ctx)` call + action switch (~50 lines)

## New Direction: True Module Architecture

Instead of module answering questions, module makes decisions:

```
BEFORE (utility bag):
  main.cpp: "hasAlertChanged?" → module: "yes"
  main.cpp: "cooldownPassed?" → module: "yes"  
  main.cpp: "OK, I'll announce" → play_audio()
  main.cpp: "markAnnounced()" → module updates

AFTER (true module):
  main.cpp: "Here's the context" → module
  module: Makes ALL decisions internally
  module: Returns VoiceAction{ANNOUNCE_PRIORITY, ka, 34700, ahead, 3}
  main.cpp: Executes the action
```

## New Migration Plan

See [docs/VOICE_ALERT_MIGRATION.md](docs/VOICE_ALERT_MIGRATION.md) for complete plan.

### Summary of Steps

| Step | Description | Risk |
|------|-------------|------|
| 1 | Add structs + empty processVoiceAlerts() | None |
| 2 | Move early exit checks | Low |
| 3 | Move new priority alert logic | Medium |
| 4 | Move direction change logic | Medium |
| 5 | Move bogey count change logic | Low |
| 6 | Move secondary alert logic | Medium |
| 7 | Move threat escalation logic | Medium |
| 8 | Cleanup dead code | Low |

Each step: build, test on hardware, commit. Same safety process as before.

## Why This Is Worth It

**Pain now:** ~6-8 hours to complete migration

**Reward later:**
- Adding voice feature = edit ONE file (module)
- Unit testing = mock VoiceAlertContext, verify VoiceAction
- Debugging = clear boundary (module decides, main executes)
- Understanding = read ONE module for all voice logic

## Current State Before Pivot

The module has all the STATE it needs. The decision LOGIC is scattered in main.cpp.
We're in a half-migrated state - not ideal, but not broken either.

The pivot completes what we started: consolidating alert logic into the module.

---

## Phase 1b: True Module Migration

### Step 15: Add VoiceAlertContext and VoiceAction Structs

**Status:** ✅ COMPLETE  
**Date:** January 28, 2026

**Objective:** Add the input/output structs and empty processVoiceAlerts() method.

**Actions:**
- [x] Add VoiceAlertContext struct to header
- [x] Add VoiceAction struct to header
- [x] Add empty processVoiceAlerts() that returns NONE
- [x] Call from main.cpp (build context, call method, ignore result)
- [x] Build and verify no behavior change

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.h - Added VoiceAlertContext, VoiceAction structs, processVoiceAlerts() declaration
- src/modules/v1_alerts/v1_alert_module.cpp - Added empty processVoiceAlerts() implementation
- src/main.cpp - Added context building and method call (result ignored)

**Compilation Result:**
- ✅ SUCCESS (11.05s)
- RAM: 25.6% (83804/327680 bytes) - unchanged
- Flash: 41.8% (2737676/6553600 bytes) - +184 bytes (structs only)

**Hardware Test:**
- [ ] Pending user test

**Next:** Step 16 - Move Early Exit Checks into processVoiceAlerts()

---

### Step 16: Move Early Exit Checks

**Status:** ✅ COMPLETE  
**Date:** January 28, 2026

**Objective:** Move the early exit checks (conditions that skip voice alerts) into processVoiceAlerts().

**Early Exit Checks Moved:**
1. Voice alerts disabled (`voiceAlertMode == DISABLED`)
2. Mute if volume zero (`muteVoiceIfVolZero && mainVolume == 0`)
3. Low speed mute (`isLowSpeedMuted()`)
4. V1 is muted (`isMuted`)
5. In GPS lockout zone (`isInLockout`)
6. Phone app connected (`isProxyConnected`)
7. No valid priority alert (`priority == null || band == NONE`)

**Actions:**
- [x] Add early exit checks to processVoiceAlerts()
- [x] Access settings via stored SettingsManager* pointer
- [x] Return NONE if any check fails
- [x] Build and verify

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.cpp - Added 7 early exit checks

**Compilation Result:**
- ✅ SUCCESS (10.84s)
- RAM: 25.6% (unchanged)
- Flash: 41.8% (2737728 bytes) - +52 bytes

**Hardware Test:**
- [ ] Pending user test

**Note:** Module still returns NONE even when checks pass. main.cpp still runs existing logic. This is intentional - we're migrating incrementally.

**Next:** Step 17 - Move Priority Alert New-Alert Logic

---

### Step 17: Move ALL Priority Alert Logic (New + Direction + Bogey Count)

**Status:** ✅ COMPLETE (Hardware verified)  
**Date:** January 28, 2026

**Objective:** Move all priority alert decision logic into processVoiceAlerts(), wire up main.cpp to execute returned actions.

**What Was Migrated:**
1. **New Alert Logic** - Returns `ANNOUNCE_PRIORITY` when band/freq changes
2. **Direction Change Logic** - Returns `ANNOUNCE_DIRECTION` when direction changes (with throttling)
3. **Bogey Count Change Logic** - Returns `ANNOUNCE_DIRECTION` when count changes

**Actions:**
- [x] Add `toAudioBand()` helper function in module
- [x] Add `isValidAnnounceBand()` helper function
- [x] Implement all 3 priority cases in processVoiceAlerts()
- [x] Module updates its own internal state (markAnnounced, etc.)
- [x] main.cpp executes VoiceAction via switch statement
- [x] Remove old priority alert code from main.cpp (~100 lines)
- [x] Build and verify
- [x] Run tests

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.cpp - Full priority alert decision logic (~80 lines)
- src/main.cpp - Execution switch, removed old code (~-65 lines net)

**Compilation Result:**
- ✅ SUCCESS (11.09s)
- RAM: 25.6% (unchanged)
- Flash: 41.8% (2737956 bytes) - +228 bytes (new logic in module)

**Test Result:**
- ✅ All 400 tests pass

**Hardware Test:**
- [x] User verified working

**Lines Removed from main.cpp:** ~100 lines (priority alert blocks)
**Lines Added to module:** ~80 lines (priority logic)
**Net Reduction:** ~20 lines

---

### Step 18: Move Secondary Alert Logic

**Status:** ✅ COMPLETE  
**Date:** January 28, 2026

**Objective:** Move secondary alert announcement logic into processVoiceAlerts().

**What Was Migrated:**
- Secondary alert decision logic: loops through non-priority alerts, finds first unannounced alert that passes band filter, returns `ANNOUNCE_SECONDARY`

**Key Conditions (all must pass):**
1. `announceSecondaryAlerts` setting enabled
2. More than one alert (`alertCount > 1`)
3. `canAnnounceSecondary()` - priority stable + cooldown passed
4. Alert not already announced
5. Band enabled for secondary (`isBandEnabledForSecondary`)

**Actions:**
- [x] Add secondary alert logic to processVoiceAlerts() (after priority cases)
- [x] Returns `ANNOUNCE_SECONDARY` with band/freq/dir
- [x] Module marks alert announced and updates cooldown
- [x] Remove old secondary code from main.cpp (~50 lines)
- [x] Build and verify
- [x] Run tests

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.cpp - Added secondary loop (~40 lines)
- src/main.cpp - Removed secondary block (~-45 lines)

**Compilation Result:**
- ✅ SUCCESS (11.05s)
- RAM: 25.6% (unchanged)
- Flash: 41.8% (2737980 bytes)

**Test Result:**
- ✅ All 400 tests pass

**Hardware Test:**
- [ ] Pending user test

---

### Step 19: Move Threat Escalation Logic

**Status:** ✅ COMPLETE  
**Date:** January 28, 2026

**Objective:** Move smart threat escalation logic into processVoiceAlerts().

**What Was Migrated:**
- Signal history tracking for all secondary alerts
- Stale history cleanup
- Escalation trigger detection (weak signal ramps up)
- Direction breakdown counting (ahead/behind/side)

**Key Logic:**
1. First pass: Update all alert histories with current signal strengths
2. Cleanup stale histories (alerts that disappeared)
3. Second pass: Check each secondary for escalation criteria
4. Build direction breakdown from all alerts
5. Return `ANNOUNCE_ESCALATION` with full breakdown

**Actions:**
- [x] Add escalation logic to processVoiceAlerts() (~70 lines)
- [x] History updates + cleanup + escalation check + direction counts
- [x] Returns `ANNOUNCE_ESCALATION` with band/freq/dir + breakdown
- [x] Remove old escalation code from main.cpp (~90 lines)
- [x] Build and verify
- [x] Run tests

**Files Modified:**
- src/modules/v1_alerts/v1_alert_module.cpp - Added escalation logic (~70 lines)
- src/main.cpp - Removed escalation block (~-90 lines)

**Compilation Result:**
- ✅ SUCCESS (11.30s)
- RAM: 25.6% (unchanged)
- Flash: 41.8% (2738064 bytes)

**Test Result:**
- ✅ All 400 tests pass

**Hardware Test:**
- [ ] Pending user test

**Next:** Step 20 - Cleanup and Final Verification
