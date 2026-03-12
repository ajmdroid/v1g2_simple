// =============================================================================
// property_runner.h — Deterministic property test harness for Unity
//
// Provides a lightweight property-testing loop that runs a generator +
// invariant function across a fixed set of seeds and case counts.
// No shrinking — failure output includes the seed, case index, and a
// user-supplied serialization of the failing input for reproduction.
//
// Usage:
//
//   #include "support/property_runner.h"
//
//   void test_my_property(void) {
//       PropertyConfig cfg;
//       cfg.name = "confidence never underflows";
//       cfg.seeds = PROPERTY_SEEDS_PR;      // or PROPERTY_SEEDS_NIGHTLY
//       cfg.seed_count = PROPERTY_SEED_COUNT_PR;
//       cfg.cases_per_seed = PROPERTY_CASES_PR;
//
//       property_run(cfg, [](uint32_t seed, uint32_t caseIndex, PropertyRng& rng) {
//           uint8_t initial = rng.u8();
//           uint8_t decrements = rng.range(0, 300);
//           uint8_t result = simulate_decrements(initial, decrements);
//           if (result > initial) {
//               property_fail_with(seed, caseIndex,
//                   "initial=%u decrements=%u result=%u", initial, decrements, result);
//           }
//       });
//   }
//
// =============================================================================
#pragma once

#include <unity.h>
#include <cstdint>
#include <cstdio>
#include <functional>

// ── Budget constants ────────────────────────────────────────────────
// PR lane: 10 seeds × 100 cases = 1000 evaluations per property
static constexpr uint32_t PROPERTY_SEED_COUNT_PR = 10;
static constexpr uint32_t PROPERTY_CASES_PR = 100;

// Nightly lane: 100 seeds × 1000 cases = 100k evaluations per property
static constexpr uint32_t PROPERTY_SEED_COUNT_NIGHTLY = 100;
static constexpr uint32_t PROPERTY_CASES_NIGHTLY = 1000;

// Fixed seed lists — deterministic across runs
static constexpr uint32_t PROPERTY_SEEDS_PR[] = {
    1, 42, 137, 256, 1024, 65535, 999999, 0xDEADBEEF, 0xCAFE, 7
};
static constexpr uint32_t PROPERTY_SEEDS_NIGHTLY[] = {
    1, 2, 3, 5, 7, 11, 13, 17, 19, 23,
    29, 31, 37, 41, 42, 43, 47, 53, 59, 61,
    67, 71, 73, 79, 83, 89, 97, 101, 103, 107,
    109, 113, 127, 131, 137, 139, 149, 151, 157, 163,
    167, 173, 179, 181, 191, 193, 197, 199, 211, 223,
    227, 229, 233, 239, 241, 251, 256, 257, 263, 269,
    271, 277, 281, 283, 293, 307, 311, 313, 317, 331,
    337, 347, 349, 353, 359, 367, 373, 379, 383, 389,
    397, 401, 409, 419, 421, 431, 433, 439, 443, 449,
    457, 461, 463, 467, 479, 487, 491, 499, 503, 509
};

// ── Simple deterministic RNG (xorshift32) ───────────────────────────
struct PropertyRng {
    uint32_t state;

    explicit PropertyRng(uint32_t seed) : state(seed ? seed : 1) {}

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    uint8_t u8() { return static_cast<uint8_t>(next() & 0xFF); }
    uint16_t u16() { return static_cast<uint16_t>(next() & 0xFFFF); }
    uint32_t u32() { return next(); }
    int32_t i32() { return static_cast<int32_t>(next()); }

    uint32_t range(uint32_t lo, uint32_t hi) {
        if (lo >= hi) return lo;
        return lo + (next() % (hi - lo));
    }

    float f32(float lo, float hi) {
        float t = static_cast<float>(next()) / static_cast<float>(0xFFFFFFFFu);
        return lo + t * (hi - lo);
    }

    bool coin() { return (next() & 1) != 0; }
};

// ── Property configuration ──────────────────────────────────────────
struct PropertyConfig {
    const char* name;
    const uint32_t* seeds;
    uint32_t seed_count;
    uint32_t cases_per_seed;
};

// ── Failure reporting ───────────────────────────────────────────────
// Call this from inside a property body to report a failure with context.
// Prints seed + case index + user message, then calls TEST_FAIL.

#define property_fail_with(seed, caseIdx, fmt, ...)                         \
    do {                                                                     \
        char _pf_buf[512];                                                   \
        snprintf(_pf_buf, sizeof(_pf_buf),                                   \
            "Property failure: seed=%u case=%u " fmt,                        \
            (unsigned)(seed), (unsigned)(caseIdx), ##__VA_ARGS__);           \
        TEST_FAIL_MESSAGE(_pf_buf);                                          \
    } while (0)

// ── Runner ──────────────────────────────────────────────────────────
// Runs the property function for each seed × cases_per_seed.
// The body should call property_fail_with(...) on invariant violation.

using PropertyBody = std::function<void(uint32_t seed, uint32_t caseIndex, PropertyRng& rng)>;

static inline void property_run(const PropertyConfig& cfg, PropertyBody body) {
    for (uint32_t si = 0; si < cfg.seed_count; ++si) {
        uint32_t seed = cfg.seeds[si];
        PropertyRng rng(seed);
        for (uint32_t ci = 0; ci < cfg.cases_per_seed; ++ci) {
            body(seed, ci, rng);
        }
    }
}
