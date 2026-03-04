/**
 * Audio internals — shared across audio_beep TU split.
 *
 * Provides: shared state declarations (atomics, buffers, hardware handles),
 * promoted helper declarations, AUDIO_LOG macros, and constants.
 * Each companion .cpp includes this plus its own specific headers.
 */

#ifndef AUDIO_INTERNALS_H
#define AUDIO_INTERNALS_H

#include "audio_beep.h"
#include "debug_logger.h"
#include "perf_metrics.h"
#include <atomic>
#include <cstdint>
#include "driver/i2s_std.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---- Debug / logging infrastructure ----
static constexpr bool AUDIO_DEBUG_LOGS = false;

#if defined(DISABLE_DEBUG_LOGGER)
#define AUDIO_LOGF(...) do { } while(0)
#define AUDIO_LOGLN(msg) do { } while(0)
#else
#define AUDIO_LOGF(...) do { \
    if (AUDIO_DEBUG_LOGS) Serial.printf(__VA_ARGS__); \
    DBG_LOGF(DebugLogCategory::Audio, __VA_ARGS__); \
} while(0)
#define AUDIO_LOGLN(msg) do { \
    if (AUDIO_DEBUG_LOGS) Serial.println(msg); \
    DBG_LOGLN(DebugLogCategory::Audio, msg); \
} while(0)
#endif

// ---- Shared constants ----
static constexpr int AUDIO_CHUNK_SAMPLES = 1024;
static constexpr int AUDIO_STEREO_CHUNK_SIZE = AUDIO_CHUNK_SAMPLES * 2;
static constexpr int SD_AUDIO_TASK_STACK_SIZE = 4096;  // Reduced from 6144 to reclaim 8 KiB BSS
static constexpr int MAX_AUDIO_CLIPS = 12;
static constexpr unsigned long AMP_WARM_TIMEOUT_MS = 3000;

// ---- Shared struct for SD audio task params ----
// Unified type: used both as local preparation buffer and pre-allocated global.
struct SDAudioTaskParams {
    char filePaths[MAX_AUDIO_CLIPS][48];
    int numClips;
};

// ---- Shared hardware state (defined in audio_beep.cpp) ----
extern bool es8311_initialized;
extern bool i2s_initialized;
extern i2s_chan_handle_t i2s_tx_chan;
extern TaskHandle_t audioTaskHandle;

// ---- Shared atomic state (defined in audio_beep.cpp) ----
extern std::atomic<bool> audio_playing;
extern std::atomic<bool> amp_is_warm;
extern std::atomic<unsigned long> amp_last_used_ms;

// ---- Shared pre-allocated buffers (defined in audio_beep.cpp) ----
// Allocated in PSRAM at boot via audio_init_hw() to reduce internal SRAM usage.
// CPU-only buffers — i2s_channel_write() copies from src to internal DMA ring.
extern int16_t* g_stereoChunkBuffer;   // AUDIO_STEREO_CHUNK_SIZE elements
extern uint8_t* g_mulawChunkBuffer;    // AUDIO_CHUNK_SAMPLES elements

// SD audio task pre-allocated global (defined in audio_beep.cpp)
extern SDAudioTaskParams g_sdAudioTaskParams;

// Static task allocation (defined in audio_beep.cpp)
// Stack lives in PSRAM (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=1).
// TCB must remain in internal SRAM per IDF requirement.
extern StackType_t* g_sdAudioTaskStack;  // SD_AUDIO_TASK_STACK_SIZE elements
extern StaticTask_t g_sdAudioTaskTCB;

// ---- Promoted hardware helper declarations (defined in audio_beep.cpp) ----
void es8311_init();
void i2s_init();
void set_speaker_amp(bool enable);

// ---- Promoted pure function (defined in audio_voice.cpp) ----
int getGHz(AlertBand band, uint16_t freqMHz);

#endif // AUDIO_INTERNALS_H
