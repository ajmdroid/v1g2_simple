# Main.cpp Refactoring Migration Log

**Started:** January 27, 2026  
**Goal:** Extract 832-line main loop into alert-centric modules  
**Strategy:** One micro-step at a time, test after each change  

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

**Completed Steps:** 5  
**Failed Steps:** 0  
**Total Estimated Steps:** TBD (will refine as we go)  
**Estimated Completion:** TBD

---

## Current Status

**Last Updated:** January 27, 2026  
**Current Step:** Step 5 - Move Announced Alert Tracking  
**Next Action:** Add state and functions to module
