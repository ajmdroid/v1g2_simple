# Tracking: Display Cache Unification

> **Status**: Planned — not scheduled
> **Risk**: Medium-High (touches every `display_*.cpp` simultaneously)
> **Prerequisite**: `scripts/check_dirty_flag_discipline.py` contract must pass before and after

---

## Problem

The display rendering pipeline uses two overlapping cache layers that must
agree for correct behavior:

1. **DisplayDirtyFlags** — coarse invalidation signals (`dirty.bands`, etc.)
2. **File-scoped statics** — per-element last-drawn value caches (`s_bandCacheValid`, etc.)

Both layers work correctly today, but the redundancy means every render
function carries boilerplate to bridge them:

```cpp
if (dirty.bands) {
    s_bandCacheValid = false;
    dirty.bands = false;
}
```

This pattern is repeated in every `display_*.cpp` file. The
`check_dirty_flag_discipline.py` contract test guards against the most
dangerous failure mode (flag consumed but not cleared), but the duplication
itself is a maintenance burden.

## Proposed Solution

1. Move all `s_*` statics into a `DisplayElementCache` struct (one instance,
   file-scoped in `display_update.cpp` or a new `display_cache.cpp`).

2. Give each dirty flag setter a side effect that zeros the corresponding
   cache section. For example, setting `dirty.bands = true` would also set
   `cache.bands.valid = false`.

3. Remove the per-function `if (dirty.x) { ... }` bridge pattern — the
   invalidation happens at the source.

4. Replace the per-element `setAll()` with a single `cache.invalidateAll()`
   that both sets all dirty flags and resets all cache sections.

## Scope

- Every `display_*.cpp` file under `src/` (7 render files + orchestration)
- `include/display_dirty_flags.h` (struct redesign)
- `display_update.cpp` (DisplayRenderCache integration)
- All display-related tests

## Why Not Now

- Touches ~7 files simultaneously — hard to bisect regressions
- Current system works correctly; the contract test guards it
- Should be done as a dedicated effort with visual regression testing
  on hardware, not during a cleanup pass

## When To Do This

- When adding a new display element type (forces touching all files anyway)
- When display rendering performance becomes an issue (unlikely — SPI is
  the bottleneck, not cache logic)
- During a major display refactor (new screen layout, new hardware target)

---

*Filed: 2026-04-02*
*Context: V4 cleanup plan, Phase 4.3*
