# Voice Alert Module Migration Plan

## Overview

This document describes the migration of voice alert logic from `main.cpp` into a dedicated module, transforming it from a "state bag" into a true decision-making module.

**Status (Jan 28, 2026):** Completed. Voice logic now lives in `src/modules/voice/voice_module.cpp`; main.cpp builds a `VoiceContext` and executes the returned `VoiceAction`. Alert persistence now lives in `src/modules/alert_persistence/alert_persistence_module.cpp`. The historical notes below remain for reference; file paths and ownership have been updated where called out.

---

## Current State Analysis

> Note: The implementation is now in `src/modules/voice/`. The tables below are kept for historical context and describe the state before the final cut-over.

### What's in VoiceModule (State + Helpers)

```
src/modules/voice/voice_module.h  (~200 lines)
src/modules/voice/voice_module.cpp (~430 lines)
```

| Category | Items |
|----------|-------|
| **Announced Alerts** | `announcedAlertIds[]`, `announcedAlertCount` |
| **Alert History** | `alertHistories[]` (for threat escalation) |
| **Direction Throttle** | `directionChangeCount`, `directionChangeWindowStart` |
| **Priority Stability** | `lastPriorityAlertId`, `priorityStableSince`, `lastPriorityAnnouncementTime` |
| **Last Announced** | `lastVoiceAlertBand`, `lastVoiceAlertDirection`, `lastVoiceAlertFrequency`, `lastVoiceAlertBogeyCount`, `lastVoiceAlertTime` |
| **Speed Cache** | `cachedSpeedMph`, `cachedSpeedTimestamp` |

| Method Type | Methods |
|-------------|---------|
| **Query State** | `isAlertAnnounced()`, `hasAlertChanged()`, `hasDirectionChanged()`, `hasCooldownPassed()`, `hasBogeyCountChanged()`, `hasBogeyCountCooldownPassed()`, `canAnnounceSecondary()`, `shouldThrottleDirectionChange()`, `shouldAnnounceThreatEscalation()`, `isLowSpeedMuted()` |
| **Update State** | `markAlertAnnounced()`, `updateLastAnnounced()`, `updateLastAnnouncedDirection()`, `updateLastAnnouncedTime()`, `markPriorityAnnounced()`, `resetDirectionThrottle()`, `updatePriorityStability()`, `resetPriorityStability()`, `updateAlertHistory()`, `markThreatEscalationAnnounced()`, `cleanupStaleHistories()` |
| **Static Helpers** | `getAlertBars()`, `makeAlertId()`, `isBandEnabledForSecondary()`, `toAudioDirection()` |
| **Combined** | `clearAllAlertState()`, `clearAnnouncedAlerts()`, `clearAlertHistories()`, `resetLastAnnounced()` |

### What's in main.cpp (post-migration)

- Builds `VoiceContext` from parser state and settings
- Calls `voiceModule.process(ctx)` once per loop
- Executes the returned `VoiceAction` via `play_frequency_voice`, `play_direction_only`, or `play_threat_escalation`
- Clears module state when alerts clear

---

## Target Architecture

### New Interface

```cpp
// ===== INPUT STRUCT =====
struct VoiceAlertContext {
    // Alert data (required)
    const AlertData* alerts;          // All current alerts
    int alertCount;                   // Number of alerts
    const AlertData* priority;        // Priority alert (can be null)
    
    // V1 state
    bool isMuted;                     // V1 is muted
    bool isProxyConnected;            // Phone app connected
    uint8_t mainVolume;               // Current V1 volume
    
    // Environment
    bool isInLockout;                 // Priority alert in lockout zone
    
    // Time
    unsigned long now;                // Current millis()
};

// ===== OUTPUT STRUCT =====
struct VoiceAction {
    enum Type { 
        NONE,                         // Do nothing
        ANNOUNCE_PRIORITY,            // Full: band + freq + dir + bogeys
        ANNOUNCE_DIRECTION,           // Just: dir + bogeys  
        ANNOUNCE_SECONDARY,           // Full for secondary alert
        ANNOUNCE_ESCALATION           // Threat ramping up
    };
    Type type = NONE;
    
    // Payload (interpretation depends on type)
    AlertBand band;                   // Band to announce
    uint16_t freq;                    // Frequency to announce
    AlertDirection dir;               // Direction to announce
    uint8_t bogeyCount;               // Bogey count (0 = don't announce)
    
    // Escalation-specific
    uint8_t aheadCount;
    uint8_t behindCount;
    uint8_t sideCount;
};

// ===== THE KEY METHOD =====
// This replaces all 250 lines of decision logic in main.cpp
VoiceAction VoiceModule::process(const VoiceAlertContext& ctx);
```

### main.cpp After Migration

```cpp
// Build context (10 lines)
VoiceAlertContext ctx;
ctx.alerts = currentAlerts.data();
ctx.alertCount = alertCount;
ctx.priority = priority.isValid ? &priority : nullptr;
ctx.isMuted = state.muted;
ctx.isProxyConnected = bleClient.isProxyClientConnected();
ctx.mainVolume = state.mainVolume;
ctx.isInLockout = priorityInLockout;
ctx.now = millis();

// Get decision from module (1 line)
VoiceAction action = voiceModule.process(ctx);

// Execute action (20 lines)
switch (action.type) {
    case VoiceAction::ANNOUNCE_PRIORITY:
        play_frequency_voice(action.band, action.freq, action.dir,
                             alertSettings.voiceAlertMode, 
                             alertSettings.voiceDirectionEnabled,
                             action.bogeyCount);
        break;
    case VoiceAction::ANNOUNCE_DIRECTION:
        play_direction_only(action.dir, action.bogeyCount);
        break;
    case VoiceAction::ANNOUNCE_SECONDARY:
        play_frequency_voice(action.band, action.freq, action.dir,
                             alertSettings.voiceAlertMode,
                             alertSettings.voiceDirectionEnabled, 1);
        break;
    case VoiceAction::ANNOUNCE_ESCALATION:
        play_threat_escalation(action.band, action.freq, action.dir,
                               action.bogeyCount, action.aheadCount,
                               action.behindCount, action.sideCount);
        break;
    case VoiceAction::NONE:
    default:
        break;
}
```

---

## Migration Steps

Each step: implement, build, test on hardware, commit.

### Step 1: Add Structs + Empty Method (Infrastructure)
**Risk: None** - No behavior change

- Add `VoiceAlertContext` struct to header
- Add `VoiceAction` struct to header  
- Add empty `process()` that returns `NONE`
- Wire up in main.cpp (call it, ignore result)
- Build and verify existing behavior unchanged

### Step 2: Move Early Exit Checks
**Risk: Low** - Simple boolean logic

Move these checks into module:
- `voiceAlertMode == DISABLED`
- `muteVoiceIfVolZero && volume == 0`
- `isLowSpeedMuted()`
- `isMuted`
- `isInLockout`
- `isProxyConnected`
- `priority == null || band == NONE`

Module returns `NONE` for all skip cases.
main.cpp: `if (action.type != NONE) { execute... }`

### Step 3: Move Priority Alert - New Alert Logic
**Risk: Medium** - Core announcement logic

Move Block 3 into module:
- Check `hasAlertChanged() && hasCooldownPassed()`
- Build `VoiceAction{ANNOUNCE_PRIORITY, ...}`
- Update internal state (markAnnounced, etc.)
- Return the action

main.cpp executes if `action.type == ANNOUNCE_PRIORITY`

### Step 4: Move Priority Alert - Direction Changed Logic  
**Risk: Medium** - Includes throttling

Move Block 4 into module:
- Check direction changed + cooldown + direction enabled
- Check throttle
- Build `VoiceAction{ANNOUNCE_DIRECTION, ...}` or `NONE` if throttled
- Update internal state

### Step 5: Move Priority Alert - Bogey Count Changed Logic
**Risk: Low** - Simple logic

Move Block 5 into module:
- Check bogey count changed + cooldown + feature enabled
- Build `VoiceAction{ANNOUNCE_DIRECTION, ...}`
- Update internal state

### Step 6: Move Secondary Alert Logic
**Risk: Medium** - Loops over alerts

Move Block 6 into module:
- Check conditions (not priority announced, feature enabled, multiple alerts, stable)
- Loop to find first unannounced secondary
- Build `VoiceAction{ANNOUNCE_SECONDARY, ...}`
- Update internal state

### Step 7: Move Threat Escalation Logic
**Risk: Medium** - Most complex block

Move Block 7 into module:
- Update all alert histories
- Cleanup stale histories
- Loop to find escalation candidate
- Count bogeys by direction
- Build `VoiceAction{ANNOUNCE_ESCALATION, ...}`
- Update internal state

### Step 8: Cleanup
**Risk: Low** - Remove dead code

- Remove old decision logic from main.cpp
- Remove now-unused public methods from module (or make private)
- Final verification

---

## Settings Dependencies

The module needs access to these settings for decisions:

| Setting | Used For |
|---------|----------|
| `voiceAlertMode` | Skip if DISABLED, pass to audio |
| `muteVoiceIfVolZero` | Skip if enabled and vol=0 |
| `voiceDirectionEnabled` | Skip direction-only if disabled |
| `announceBogeyCount` | Include count in announcements |
| `announceSecondaryAlerts` | Enable secondary + escalation |
| `secondaryLaser/Ka/K/X` | Band filter for secondary |
| `lowSpeedMuteEnabled` | Enable low-speed mute |
| `lowSpeedMuteThresholdMph` | Speed threshold |

**Note:** Module already has `SettingsManager*` from `begin()`. Can access via `settings->get()`.

---

## Audio Function Signatures (For Reference)

```cpp
// Full announcement: "Ka 34.7 ahead, 3 bogeys"
void play_frequency_voice(AlertBand band, uint16_t freq, AlertDirection dir,
                          VoiceAlertMode mode, bool includeDirection, 
                          uint8_t bogeyCount);

// Direction only: "ahead, 3 bogeys" or just "ahead"  
void play_direction_only(AlertDirection dir, uint8_t bogeyCount);

// Escalation: "Ka 34.7 ahead, 4 bogeys, 2 ahead, 1 behind"
void play_threat_escalation(AlertBand band, uint16_t freq, AlertDirection dir,
                            uint8_t totalBogeys, uint8_t aheadCount,
                            uint8_t behindCount, uint8_t sideCount);
```

---

## Testing Checklist

After each step, verify on hardware:

- [ ] Build succeeds
- [ ] Device boots normally
- [ ] Priority alert announced on new alert
- [ ] Direction change announced (not throttled)
- [ ] Direction change throttled after 3 bounces
- [ ] Bogey count change announced
- [ ] Secondary alert announced after priority stable
- [ ] Threat escalation announced when weak→strong
- [ ] No announcement when muted
- [ ] No announcement when in lockout
- [ ] No announcement when phone app connected
- [ ] No announcement when volume is 0 (if setting enabled)
- [ ] No announcement at low speed (if setting enabled)

---

## Rollback Plan

If any step causes issues:
1. Git revert the commit
2. Rebuild
3. Analyze what went wrong
4. Try again with smaller change

Each step is independent and reversible.

---

## Estimated Timeline

| Step | Effort | Risk |
|------|--------|------|
| Step 1 | 30 min | None |
| Step 2 | 30 min | Low |
| Step 3 | 1 hour | Medium |
| Step 4 | 45 min | Medium |
| Step 5 | 30 min | Low |
| Step 6 | 1 hour | Medium |
| Step 7 | 1.5 hours | Medium |
| Step 8 | 30 min | Low |

**Total: ~6-8 hours** spread across sessions

---

## Success Criteria

After migration complete:

1. **main.cpp voice logic:** ~30 lines (build context + execute action)
2. **Module `VoiceModule::process()`:** ~200 lines (all decisions)
3. **Behavior:** Identical to before
4. **Testability:** Can unit test with mock VoiceAlertContext
