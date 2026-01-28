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
**Status:** 🟡 IN PROGRESS  
**Date:** January 27, 2026  
**Commit:** [pending]

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
- ✅ No crashes or freezes

**Issues Encountered:**
- None

**Rollback Required:**
- No

---

## Step 2: Add References to External Objects

**Status:** 🟡 IN PROGRESS  
**Date:** January 27, 2026
**Commit:** [pending]

**Objective:** Give the module access to external objects it will need (bleClient, parser, display, etc.) without moving any logic yet.

**Actions:**
- [ ] Add references/pointers to V1AlertModule for: bleClient, parser, display, settingsManager
- [ ] Pass them in begin() or via setters
- [ ] Verify compilation
- [ ] Verify hardware still works identically

**Files Modified:**
- 

**Compilation Result:**
- 

**Hardware Test Result:**
-

**Compilation Result:**
- 

**Hardware Test Result:**
- 

**Issues Encountered:**
- 

**Rollback Required:**
- 

---

### Step 2: [Next step - TBD after Step 1 completes]

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

**Completed Steps:** 0  
**Failed Steps:** 0  
**Total Estimated Steps:** TBD (will refine as we go)  
**Estimated Completion:** TBD

---

## Current Status

**Last Updated:** January 27, 2026  
**Current Step:** Step 1 - Create Module Structure  
**Next Action:** Create directories and stub files
