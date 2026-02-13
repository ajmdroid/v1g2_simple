# Logging Convention

This project currently has mixed logging styles. Use these rules for new or touched code:

1. Prefix format: `[AREA]` in uppercase (example: `[BLE]`, `[WIFI]`, `[PERF]`).
2. Include level keywords for failures (`ERROR`, `WARN`) when appropriate.
3. Use stable prefixes per subsystem and avoid introducing alternate spellings.
4. Keep legacy routes and behavior, but mark compatibility paths with explicit logs when used.
5. For high-frequency paths, use debug-gated logs; for actionable failures, use always-visible logs.

These rules are additive and should be applied incrementally in small commits.
