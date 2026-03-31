#pragma once
/**
 * recording_canvas.h
 *
 * Phase 2 Task 2.1 — A thin alias/utility header providing the
 * GFX-recording Arduino_Canvas that is already embedded in display_driver.h.
 *
 * Rationale: display_driver.h exposes all the recording infrastructure
 * (fillRectCalls, fillTriangleCalls, clearRecordedCalls(), etc.) directly
 * on Arduino_Canvas.  This header makes the dependency explicit and provides
 * the helper type alias RecordingCanvas for tests that want a clearer name.
 *
 * Usage:
 *   #include "../mocks/recording_canvas.h"
 *   ...
 *   Arduino_Canvas canvas(640, 172, nullptr);
 *   // or:
 *   RecordingCanvas canvas(640, 172, nullptr);
 */

#include "display_driver.h"

/// Alias for Arduino_Canvas that emphasises its GFX-recording role.
using RecordingCanvas = Arduino_Canvas;
