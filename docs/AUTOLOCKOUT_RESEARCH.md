# Autolockout / GPS Lockout Research

> **Temporary research document** — gathered from rdforum.org, JBV1, V1 Connection, Vortex Radar, valentine1.com, Wikipedia, GitHub, and the existing v1g2_simple codebase.

---

## Table of Contents

1. [What Is a GPS Lockout / Autolockout?](#1-what-is-a-gps-lockout--autolockout)
2. [Why Lockouts Exist — The False Alert Problem](#2-why-lockouts-exist--the-false-alert-problem)
3. [How GPS Lockouts Work — Core Algorithm](#3-how-gps-lockouts-work--core-algorithm)
4. [Manual vs. Automatic Lockouts](#4-manual-vs-automatic-lockouts)
5. [Valentine One & The GPS Controversy](#5-valentine-one--the-gps-controversy)
6. [Third-Party Apps — JBV1 and V1Driver](#6-third-party-apps--jbv1-and-v1driver)
7. [Competing Implementations — Escort, Uniden, Radenso](#7-competing-implementations--escort-uniden-radenso)
8. [What Makes a Good & Safe Autolockout System](#8-what-makes-a-good--safe-autolockout-system)
9. [Known Risks & Failure Modes](#9-known-risks--failure-modes)
10. [Existing v1g2_simple Lockout Architecture](#10-existing-v1g2_simple-lockout-architecture)
11. [Design Recommendations for Our Implementation](#11-design-recommendations-for-our-implementation)

---

## 1. What Is a GPS Lockout / Autolockout?

A **GPS lockout** (also called **autolockout**, **GPS filtering**, or **GPS false-alert filtering**) is a system that uses the driver's GPS coordinates to identify and suppress known stationary false radar alerts at specific locations.

**Core concept:** If a radar detector encounters the same signal at the same GPS location multiple times on separate drives, it is almost certainly a stationary source (automatic door opener, speed sign, security system, etc.) rather than a police speed trap. The system records the location + frequency and automatically mutes/suppresses the alert on future passes.

**Manual lockout:** The driver presses a button to instantly mark a known false alert at the current GPS position.

**Autolockout (automatic lockout):** The system learns false alerts automatically over multiple passes without driver intervention; after encountering the same signal at the same location a configurable number of times (typically 2–3 passes), it auto-promotes the location as a lockout zone.

---

## 2. Why Lockouts Exist — The False Alert Problem

Modern radar detectors face an epidemic of false alerts from non-police sources operating on the same radar bands (primarily K band at ~24.1 GHz):

### Common Sources of False K-Band Alerts
| Source | Frequency | Behavior |
|--------|-----------|----------|
| **Automatic door openers** (grocery stores, pharmacies, big box stores) | K band (~24.125 GHz) | Stationary, low-power, frequency drifts with temperature |
| **Speed advisory signs** ("Your Speed Is…" signs) | K band | Stationary, always-on |
| **Blind Spot Monitoring (BSM)** systems | K band (~24.1 GHz) | **Moving** false alerts from other vehicles |
| **Adaptive Cruise Control** (some GM, Mercedes) | K band | Moving false alerts |
| **Rear Cross-Traffic Warning (RCTW)** | K band | Moving, triggered in reverse |
| **Security/intrusion sensors** | K band | Stationary |
| **Traffic flow sensors** | K band | Stationary, roadside |

### Why This Matters
- In urban/suburban environments, a radar detector without false-alert filtering can alert **hundreds of times per drive**, rendering it effectively useless — drivers learn to ignore alerts ("cry wolf" effect)
- Valentine One founder Mike Valentine: *"The only thing worse than detecting a false signal is failing to detect real radar"* — but incessant false alerts lead to exactly that outcome through desensitization
- GPS lockouts let the detector stay quiet in known-false locations while remaining fully alert for genuine threats

---

## 3. How GPS Lockouts Work — Core Algorithm

### The Learning / Promotion Pipeline

```
Pass 1: Signal detected at (lat, lon) on K-band @ 24.150 GHz
        → New learner candidate created: {location, band, freq, hitCount=1}
        → No suppression yet

Pass 2: Same signal detected within radius (~150m) and freq tolerance (~10 MHz)
        → hitCount incremented to 2
        → Still no suppression

Pass 3: hitCount reaches promotion threshold (e.g., 3)
        → Candidate PROMOTED to full lockout zone
        → Future visits: alert suppressed/muted automatically
```

### Key Parameters

| Parameter | Typical Value | Purpose |
|-----------|---------------|---------|
| **Promotion threshold** | 2–3 passes | Minimum sightings before auto-lockout |
| **Spatial radius** | 100–400 meters | How close you must pass to "match" a zone |
| **Frequency tolerance** | ±5–20 MHz | How close the signal frequency must match |
| **Separate drives** | Time-gated (hours) | Each hit must be on a separate drive/session |
| **Band policy** | K + X only (default) | Which bands can be locked out |

### Spatial Matching

Most implementations use **simplified distance calculations** (no full haversine) for real-time performance:
- Convert lat/lon to fixed-point integers (×100,000 → "E5" format)
- Use bounding-box pre-filter + squared-distance comparison
- Typical matching radius: ~135 E5 units ≈ 150 meters

### Frequency Matching

- Door openers and speed signs operate on specific but non-precision frequencies
- Signals must match within a configurable tolerance (e.g., ±10 MHz)
- This prevents a lockout for one source from suppressing a different source at the same location on a different frequency

---

## 4. Manual vs. Automatic Lockouts

### Manual Lockouts
- **Driver presses a button** to mark the current alert as a known false
- Takes effect immediately — no learning period
- Best for locations the driver already knows well
- Risk: driver might accidentally lock out a real police location
- Some systems include **manual demotion** — if the zone isn't seen for N passes, it auto-demotes

### Automatic Lockouts (Autolockouts)
- **System learns over multiple passes** — no driver intervention
- Safer: requires repeated sightings, reducing chance of locking out a moving/temporary source
- Slower: takes 2–3 drives past the same location before taking effect
- Better for commuters who drive the same routes daily
- The "gold standard" for daily-driver false-alert filtering

### Hybrid Approach (Best Practice)
- Most modern systems support **both** manual and auto lockouts
- Manual for known-false locations you encounter immediately
- Auto for gradual learning of your daily environment
- Both types should be subject to **demotion/unlearning** if the source disappears

---

## 5. Valentine One & The GPS Controversy

### Valentine's Position: No Built-In GPS

The Valentine One Gen2 **deliberately omits GPS** from the detector itself. Mike Valentine's argument (from valentine1.com/v1-info/tech-reports/mike-on-gps/):

> *"GPS-based signal blocking has a fatal flaw on stationary alarms... In yesterday's world, there was no technical difference between the signals we call 'false alarms' and those of real radar. So a location-based blocking system blocks real radar just as eagerly as it blocks unwanted alerts."*

**Mike's specific concerns:**

1. **Sub-band overlap risk:** GPS lockouts suppress a frequency sub-band within a geographic zone. If a real police radar gun operates within the same sub-band at the same location, it gets suppressed too. (Demonstrated by Guys of LIDAR: an Escort 9500i locked out K-band police radar near a Shell station with a blocked door opener.)

2. **Frequency drift:** Cheap door opener transmitters are non-precision devices. Their frequency drifts with temperature changes, potentially moving out of the locked-out sub-band — causing the false alert to return, which leads the user to lock out a second sub-band, doubling the Murphy's Law risk.

3. **Moving falses:** Most modern K-band false alerts come from Blind Spot Monitoring in other vehicles, which are **moving** sources that GPS lockouts can't address.

**Valentine's alternative approach:**
- **K-Verifier (SAW D2L):** Pattern-based signal analysis that distinguishes real police radar from false alerts by signal characteristics, not location
- **SAVVY / eSAVVY:** Speed-based muting — lowers volume below a speed threshold instead of suppressing
- **Logic Modes (l/L):** Signal-strength-based filtering — weak signals muted in Advanced Logic mode

### The Community Response

Despite Mike Valentine's concerns, the radar detector community overwhelmingly values GPS lockouts, particularly through third-party apps. The V1 Gen2's **open Bluetooth API** enables apps like JBV1 and V1Driver to add GPS lockout functionality with **better safeguards** than built-in GPS detectors:

> *"The third party apps available for the V1 that add in GPS lockout functionality (V1Driver and JBV1) are designed more intentionally and with built-in safeguards to help virtually eliminate that risk."* — Vortex Radar

### Patent Considerations

GPS lockout technology is **patented by Escort** (purchased from a patent troll who had previously sued Escort). This creates legal risk for any manufacturer building GPS lockouts directly into hardware, which is another reason Valentine avoids built-in GPS.

---

## 6. Third-Party Apps — JBV1 and V1Driver

### JBV1 (Android only, free)

JBV1 by @johnboy00 is considered the **best countermeasure app available** (Vortex Radar). It's optimized for the Valentine One and provides:

**Lockout-specific features:**
- **Automatic GPS lockouts** — learns stationary false alerts over multiple passes
- **Can visually chill out the V1's display** in shopping centers
- **Additional BSM recognition** — catches Honda/Acura/Mazda/GM BSM falses that punch through V1's K-Verifier
- **Low speed muting** via phone GPS
- **Frequency display** of radar alerts (critical for lockout decisions)
- **Alert logging** — historical data for analysis

**Additional features:**
- Real-time crowd-sourced police alerts
- Historical crowd-sourced speed trap data overlaid on map
- Red light camera / speed camera alerts
- Speed limit display
- Auto-reprogramming of V1 by location (e.g., enable X band in Ohio, disable K in California)
- Police aircraft overhead alerts
- TMG laser jammer integration
- Works in background with small overlay pop-ups

**Key safety features in JBV1's lockout system:**
- Configurable promotion threshold
- Band restrictions (Ka lockouts typically disabled or restricted)
- Auto-demotion of stale lockouts
- Reconnects automatically when driver enters vehicle

### V1Driver (iOS + Android, $10)

V1Driver by Softronix provides:
- Automatic GPS lockouts that **learn your stationary false alerts**
- Low speed muting
- Voice announcements
- Frequency display
- Alert logging
- Apple Watch integration (visual alerts + mute via wrist)
- **Seamless background operation** — connects and runs without driver interaction

> *"V1Driver will learn your stationary false alerts from speed signs and door openers and automatically begin muting them for you."* — Vortex Radar

---

## 7. Competing Implementations — Escort, Uniden, Radenso

### Escort (Redline 360c, Max 360c MKII)
- **AutoLearn** — built-in autolockout learning over 3 passes
- **GPS-based lockouts** directly in detector hardware (patented)
- User-defined lockouts via button press
- Frequency sub-band based locking (Mike V's criticism target)
- Connected to Escort Live for crowd-sourced data

### Uniden (R8, R7, R4, R3)
- **GPS lockouts** in GPS-equipped models (R8, R7, R4)
- Manual lockout via button press; auto-lockout via AutoMute Memory
- Frequency displayed on OLED screen
- Third-party firmware community (R7/R3) for enhanced lockout features
- More granular frequency matching than Escort

### Radenso (Pro M, RC M, DS1)
- **GPS lockouts** with auto-learning
- Excellent BSM filtering (best in class for K-band false rejection)
- K-band MultaRadar detection (MRCD/MRCT)
- Red light/speed camera database integration

### Key Differences

| Feature | Escort | Uniden | Radenso | V1 + JBV1/V1Driver |
|---------|--------|--------|---------|---------------------|
| Built-in GPS lockout | Yes | Yes (GPS models) | Yes | No — app-based |
| Autolockout | 3 pass | Yes | Yes | Yes (via app) |
| Manual lockout | Yes | Yes | Yes | Yes (via app) |
| Frequency precision | Sub-band | Good | Good | Excellent (via BLE data) |
| Ka lockout | Limited | Optional | Optional | Optional (disabled by default) |
| Demotion/unlearn | Limited | Some | Some | Configurable |
| Phone required | No | No | No | **Yes** |

---

## 8. What Makes a Good & Safe Autolockout System

Based on community consensus, Vortex Radar analysis, JBV1 design philosophy, Mike Valentine's concerns, and studying implementations:

### MUST-HAVE Safety Features

1. **Ka band lockouts DISABLED by default**
   - Ka band is almost exclusively used for police radar (33.8, 34.7, 35.5 GHz)
   - Locking out Ka risks missing real enforcement
   - If enabled, should require explicit opt-in with a clear safety warning
   - Some photo radar/red light cameras use Ka — only advanced users should lock these

2. **Laser NEVER lockable**
   - Laser is always a real threat — never from false sources worth locking out
   - Must be hardcoded as non-lockable in the band policy

3. **Minimum 2–3 pass promotion threshold**
   - Single-pass lockout is too aggressive — could lock out a one-time police setup
   - 3 passes is the community standard for safety
   - Passes should be **time-separated** (hours apart, not minutes) to confirm truly stationary

4. **Frequency matching with tolerance**
   - Lock out by GPS location + frequency band + specific frequency (±tolerance)
   - A lockout for a door opener at 24.150 GHz should NOT suppress police K-band at 24.200 GHz
   - Tighter frequency tolerance = safer but might miss temperature-drifted sources

5. **Auto-demotion / clean-pass decay**
   - If the source disappears (store closes, sign removed), the lockout must automatically remove itself
   - Driving through a lockout zone without triggering should count as a "clean pass"
   - After enough clean passes, the zone should be demoted/removed
   - Prevents "ghost lockouts" that persist long after the source is gone

6. **Core system health guard**
   - Lockout processing must **never** compromise BLE connectivity or alert ingestion
   - If the system is under stress (queue drops, perf degradation), lockout enforcement should automatically disable
   - Priority order: V1 connectivity > BLE ingest > Display > Audio > Lockouts

7. **Ka/Laser override — always break through**
   - Even if a zone is locked out, any Ka or Laser alert must **force-unmute** the detector
   - This is the nuclear safety valve — Ka/Laser should never be suppressed by a lockout

### SHOULD-HAVE Features

8. **Progressive modes: Shadow → Advisory → Enforce**
   - Shadow: evaluate but never suppress; log everything — lets users see what *would* happen
   - Advisory: show lockout status visually but don't suppress audio
   - Enforce: full suppression
   - This lets users build confidence before trusting the system

9. **GPS quality gate**
   - No lockout evaluation without a valid GPS fix
   - Require minimum satellite count and HDOP quality
   - Invalid GPS should fail-safe to "alert on everything"

10. **Separate-drive interval gating**
    - Learning hits should be separated by a configurable time interval (e.g., 4+ hours)
    - Prevents counting multiple passes during a single parking-lot visit as separate events
    - Critical for correctly identifying truly stationary sources

11. **Reasonable radius (100–400m)**
    - Too small: miss the source on slightly different approaches
    - Too large: risk blocking real threats near a false source
    - ~150m default is widely accepted as balanced
    - Should be configurable per-zone and globally

12. **Import/Export capability**
    - Allow users to share lockout databases
    - Back up before reset/changes
    - Migrate between devices/vehicles

13. **Manual zone management UI**
    - Create, edit, delete individual zones
    - View all zones on a list/map
    - See learner candidates and their progress toward promotion

### NICE-TO-HAVE Features

14. **Pre-quiet volume drop**
    - When approaching a known lockout zone, proactively lower volume
    - If a genuine alert occurs, immediately restore full volume
    - Smooths the user experience in high-false-alert areas

15. **Direction-aware lockouts**
    - Some false sources are only visible from certain approach directions
    - A door opener might only alert when driving eastbound past a shopping center
    - Direction-aware lockouts reduce the zone "footprint" and minimize suppression risk

16. **Signal observation logging**
    - CSV/JSON logging of all signal observations for offline analysis
    - Helps diagnose issues and tune parameters
    - Useful for developers and power users

17. **Crowd-sourced lockout databases**
    - Community-shared known false-alert locations
    - Pre-populates lockouts for new users
    - Must still respect local verification (don't trust blindly)

---

## 9. Known Risks & Failure Modes

### Critical Risks

| Risk | Scenario | Mitigation |
|------|----------|------------|
| **Locking out real police radar** | Police sets up a speed trap near a door opener on the same K-band frequency | Frequency tolerance matching, Ka override, clean-pass demotion |
| **Stale lockout at changed location** | Store closes, lockout persists, police now operate there | Auto-demotion after N clean passes |
| **Ka band lockout** | User locks out Ka photo radar; police with same Ka freq nearby | Ka lockouts disabled by default; explicit warning |
| **GPS inaccuracy** | Poor GPS fix creates lockout at wrong location | GPS quality gate, minimum satellite count, HDOP check |
| **Frequency drift** | Door opener frequency drifts with temperature; user re-locks out adjacent frequency sub-band | Tighter frequency tolerance, periodic re-evaluation |
| **Over-reliance** | User trusts lockouts completely, ignores visual indicators | Advisory mode, visual lockout indicators, Ka/Laser always alert |
| **System overload** | Lockout processing degrades BLE/display performance | Core health guard, bounded processing time, degrade gracefully |

### The "Murphy's Law" Problem (Mike Valentine)

Mike Valentine's core argument deserves specific attention:

1. **A lockout at GPS position X on sub-band Y will suppress ALL signals at X on Y** — including real police radar
2. **The probability is non-zero** — demonstrated by Guys of LIDAR with Escort 9500i
3. **Frequency drift compounds the risk** — door openers drift, leading to multiple sub-band lockouts at one location

**Mitigations that address Mike's concern:**
- Use **narrow frequency tolerance** (±5–10 MHz) instead of broad sub-band suppression
- **Ka/Laser always break through** regardless of lockout status
- **Auto-demotion** removes stale lockouts
- **Shadow/Advisory modes** let users see the system's behavior before trusting it
- **Per-frequency** matching rather than per-sub-band matching significantly reduces collision probability

---

## 10. Existing v1g2_simple Lockout Architecture

The repo already has a **comprehensive, production-quality lockout system** with all major safety features implemented.

### Module Structure

```
src/modules/lockout/
├── lockout_entry.h           # Core zone data structure
├── lockout_index.h/.cpp      # 200-slot flat array with O(N) spatial query
├── lockout_enforcer.h/.cpp   # Per-frame evaluation + mute/unmute decisions
├── lockout_learner.h/.cpp    # 64-candidate auto-learning pipeline
├── lockout_store.h/.cpp      # SD card JSON persistence
├── lockout_band_policy.h/.cpp# Band mask gate (K+X default, Ka opt-in)
├── lockout_runtime_mute_controller.h/.cpp  # BLE mute state machine
├── lockout_pre_quiet_controller.h/.cpp     # Pre-emptive volume drop
├── lockout_orchestration_module.h/.cpp     # Top-level pipeline
├── signal_capture_module.h/.cpp            # BLE frame → signal observation
├── signal_observation_log.h/.cpp           # Thread-safe ring buffer (256 slots)
├── signal_observation_sd_logger.h/.cpp     # Async CSV logging
└── lockout_api_service.h/.cpp              # REST API for web UI

src/modules/gps/
└── gps_lockout_safety.h/.cpp  # Core-guard health evaluator
```

### Lockout Zone Data Structure

```cpp
struct LockoutEntry {
    int32_t  latE5, lonE5;     // Fixed-point GPS (degrees × 100,000)
    uint16_t radiusE5;          // ~135 = 150m
    uint8_t  bandMask;          // K=0x04, X=0x08, Ka=0x02 (opt-in), Laser=0x01 (never)
    uint16_t freqMHz;           // Center frequency
    uint16_t freqTolMHz;        // ±tolerance
    uint8_t  confidence;        // 0–255 (grows on hit, decays on miss)
    uint8_t  flags;             // ACTIVE | MANUAL | LEARNED
    uint8_t  directionMode;     // ALL / FORWARD / REVERSE
    int16_t  headingDeg;        // Directional matching
    uint16_t headingTolDeg;
    uint8_t  missCount;         // Clean-pass streak counter
    uint64_t firstSeenMs, lastSeenMs, lastPassMs, lastCountedMissMs;
};
```

### Operational Modes

| Mode | Behavior |
|------|----------|
| **OFF** | No lockout evaluation |
| **SHADOW** | Evaluate + log, never suppress (read-only on index) |
| **ADVISORY** | Visual display of lockout matches, no audio suppression |
| **ENFORCE** | Full suppression: mutes V1, updates confidence, records passes |

### Safety Mechanisms Already Implemented

1. **Core Guard** — blocks enforcement if BLE queue drops, perf drops, or event bus drops exceed thresholds (default: any drop = guard trips)
2. **Ka/Laser Force-Unmute** — if Ka or Laser detected while V1 muted by lockout → force-unmute with retry (15 × 400ms)
3. **Ka Learning Default Off** — must opt-in; UI shows warning modal
4. **Laser Never Lockable** — hardcoded strip from band mask
5. **Progressive Modes** — Shadow → Advisory → Enforce
6. **GPS Required** — no evaluation without valid fix
7. **Clean-Pass Auto-Demotion** — configurable miss threshold and interval
8. **Proxy Session Bypass** — when phone app (JBV1/V1Driver) is connected, device lockout enforcement defers

### Web UI (lockouts page)

Full-featured management interface:
- GPS status display (fix, satellites, speed)
- Mode selector (Off/Shadow/Advisory/Enforce)
- Live signal observation feed
- Active zones table (paginated, with lat/lon, radius, band, freq, confidence, direction, miss count)
- Pending learner candidates
- Zone CRUD (create, edit, delete)
- Configuration panel with presets ("Legacy Safe", "Balanced Blend")
- Import/Export
- Advanced settings toggle

### REST API

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/lockouts/summary` | GET | Stats + latest observation |
| `/api/lockouts/events` | GET | Recent observations (paginated) |
| `/api/lockouts/zones` | GET | Active zones + pending (paginated) |
| `/api/lockouts/zones/create` | POST | Create manual zone |
| `/api/lockouts/zones/update` | POST | Update zone by slot |
| `/api/lockouts/zones/delete` | POST | Delete zone by slot |
| `/api/lockouts/zones/export` | GET | Full database export (JSON) |
| `/api/lockouts/zones/import` | POST | Import database (replaces, with rollback) |

---

## 11. Design Recommendations for Our Implementation

### What's Already Solid

The existing v1g2_simple lockout system is **extremely well-designed** and already implements nearly every best practice from the community. Key strengths:

- Integer-only spatial math (no floating-point → deterministic, fast)
- Per-frequency matching (not sub-band → addresses Mike V's core criticism)
- Ka/Laser safety overrides
- Progressive mode sequence
- Core health guard integration
- Clean-pass demotion with configurable policy
- Full REST API + web UI

### Potential Areas for Enhancement

1. **Minimum GPS quality gate** — verify HDOP/satellite thresholds are enforced before lockout evaluation (not just "has fix")
2. **Zone capacity** — 200 slots may be tight for heavy commuters; consider dynamic expansion or prioritized eviction
3. **Map visualization** — a map view of lockout zones in the web UI would be very valuable
4. **Crowd-sourced database** — import from community databases (e.g., RDFGS format from rdforum.org)
5. **Analytics dashboard** — hit rates, false-positive analysis, lockout effectiveness metrics
6. **Batch operations** — bulk delete, bulk edit, "clear all learned" zones
7. **Per-route profiles** — different lockout databases for different regular routes/vehicles

### Key Principles (from community consensus)

1. **Safety first** — it is always better to alert on a false alarm than to suppress a real threat
2. **Ka/Laser are sacred** — never suppress automatically without extreme explicit opt-in
3. **Learn slowly, forget quickly** — take time to confirm a source is truly stationary, but remove stale lockouts promptly
4. **Fail open** — any system error, GPS loss, or uncertainty → alert on everything
5. **Respect the priority stack** — lockout processing must never degrade BLE/display/audio
6. **Trust but verify** — shadow/advisory modes let users audit the system before trusting it with enforcement

---

## Sources

- **valentine1.com** — Mike Valentine on GPS: https://www.valentine1.com/v1-info/tech-reports/mike-on-gps/
- **Vortex Radar** — V1 Gen2 Review: https://www.vortexradar.com/2020/05/valentine-1-gen2-review/
- **Vortex Radar** — Getting Started with JBV1: https://www.vortexradar.com/2020/03/getting-started-with-jbv1/
- **V1 Connection app** — valentine1.com/v1-detectors/v1-radar-detector-apps/
- **rdforum.org** — Radar Detector Forum (community discussions, JBV1 section)
- **Wikipedia** — Radar Detector article
- **v1g2_simple codebase** — src/modules/lockout/, src/modules/gps/, data/lockouts.html
