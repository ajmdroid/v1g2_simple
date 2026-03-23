/**
 * Secondary alert cards — extracted from display.cpp (Phase 2L)
 *
 * Contains drawSecondaryAlertCards (mini V1 alert cards at screen bottom).
 */

#include "display.h"
#include "../include/display_layout.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "display_font_manager.h"
#include "settings.h"
#include <algorithm>
#include <cstring>

using DisplayLayout::PRIMARY_ZONE_HEIGHT;
using DisplayLayout::SECONDARY_ROW_HEIGHT;

// ---------------------------------------------------------------------------
// Secondary alert cards — mini V1 alert cards at screen bottom
// With persistence: cards stay visible (greyed) for grace period after alert ends
// ---------------------------------------------------------------------------

void V1Display::drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    secondaryCardsRenderDirty_ = false;

    const int cardH = SECONDARY_ROW_HEIGHT;  // 54px (compact with uniform signal bars)
    const int cardY = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT;  // Y=118
    const int cardW = 145;     // Card width (wider to fit freq + band)
    const int cardSpacing = 10;  // Increased spacing between cards
    const int leftMargin = 120;   // After band indicators
    const int rightMargin = 200;  // Before signal bars (at X=440)
    const int availableWidth = SCREEN_WIDTH - leftMargin - rightMargin;  // 320px
    // Center two cards in available space
    const int totalCardsWidth = cardW * 2 + cardSpacing;  // 300px
    const int startX = leftMargin + (availableWidth - totalCardsWidth) / 2;  // Center offset
    
    // Get persistence time from profile settings (same as main alert persistence)
    const V1Settings& settings = settingsManager.get();
    uint8_t persistSec = settingsManager.getSlotAlertPersistSec(settings.activeSlot);
    unsigned long gracePeriodMs = persistSec * 1000UL;
    
    // If persistence is disabled (0), cards disappear immediately
    if (gracePeriodMs == 0) {
        gracePeriodMs = 1;  // Minimum 1ms so expiration logic works
    }
    
    unsigned long now = millis();
    
    // Static card slots for persistence tracking
    static struct {
        AlertData alert{};
        unsigned long lastSeen = 0;  // 0 = empty slot
    } cards[2];
    
    // Track previous priority to add as persisted card when it disappears
    static AlertData lastPriorityForCards;
    
    // Track what was drawn at each POSITION (0 or 1) for incremental updates
    static struct {
        // V1 card state
        Band band = BAND_NONE;
        uint32_t frequency = 0;
        uint8_t direction = 0;
        bool isGraced = false;
        bool wasMuted = false;
        uint8_t bars = 0;           // Signal strength bars (0-6)
    } lastDrawnPositions[2];
    [[maybe_unused]] static int lastDrawnCount = 0;
    
    // Track profile changes - clear cards when profile rotates
    static int lastCardProfileSlot = -1;
    if (settings.activeSlot != lastCardProfileSlot) {
        lastCardProfileSlot = settings.activeSlot;
        // Clear all card state on profile change
        for (int c = 0; c < 2; c++) {
            cards[c].alert = AlertData();
            cards[c].lastSeen = 0;
            lastDrawnPositions[c].band = BAND_NONE;
            lastDrawnPositions[c].frequency = 0;
            lastDrawnPositions[c].bars = 0;
        }
        lastDrawnCount = 0;
        lastPriorityForCards = AlertData();
    }
    
    // If called with nullptr alerts and count 0, clear V1 card state
    if (alerts == nullptr && alertCount == 0) {
        for (int c = 0; c < 2; c++) {
            cards[c].alert = AlertData();
            cards[c].lastSeen = 0;
        }
        lastPriorityForCards = AlertData();
        
        // Clear the card area
        [[maybe_unused]] const int signalBarsX = SCREEN_WIDTH - 200 - 2;
        const int clearWidth = signalBarsX - startX;
        if (clearWidth > 0) {
            FILL_RECT(startX, cardY, clearWidth, cardH, PALETTE_BG);
            if (lastDrawnCount > 0) {
                secondaryCardsRenderDirty_ = true;
            }
        }
        // Reset last drawn count so next time cards appear, change is detected
        lastDrawnCount = 0;
        return;
    }
    
    // Helper: check if two alerts match (same band + frequency within tolerance)
    // V1 frequency can jitter by a few MHz between frames - use ±5 MHz tolerance
    auto alertsMatch = [](const AlertData& a, const AlertData& b) -> bool {
        if (a.band != b.band) return false;
        if (a.band == BAND_LASER) return true;
        // Use a small tolerance to handle V1 jitter without merging distinct nearby bogeys
        const uint32_t FREQ_TOLERANCE_MHZ = 2;
        uint32_t diff = (a.frequency > b.frequency) ? (a.frequency - b.frequency) : (b.frequency - a.frequency);
        return diff <= FREQ_TOLERANCE_MHZ;
    };
    
    // Helper: check if alert matches priority (returns false if priority is invalid)
    auto isSameAsPriority = [&priority, &alertsMatch](const AlertData& a) -> bool {
        if (!priority.isValid || priority.band == BAND_NONE) return false;
        return alertsMatch(a, priority);
    };
    
    // Step 0: Check if priority changed - add old priority as persisted card
    // This handles the case where laser takes priority, then stops - laser should persist as card
    if (lastPriorityForCards.isValid && lastPriorityForCards.band != BAND_NONE) {
        bool priorityChanged = !alertsMatch(lastPriorityForCards, priority);
        bool oldPriorityGone = true;
        
        // Check if old priority is still in current alerts
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(lastPriorityForCards, alerts[i])) {
                    oldPriorityGone = false;
                    break;
                }
            }
        }
        
        // If old priority is gone (not just demoted), add it as persisted card
        if (priorityChanged && oldPriorityGone) {
            // Check if already tracked
            bool found = false;
            for (int c = 0; c < 2; c++) {
                if (cards[c].lastSeen > 0 && alertsMatch(cards[c].alert, lastPriorityForCards)) {
                    found = true;
                    break;
                }
            }
            
            // Add to empty slot if not already tracked
            if (!found) {
                for (int c = 0; c < 2; c++) {
                    if (cards[c].lastSeen == 0) {
                        cards[c].alert = lastPriorityForCards;
                        cards[c].lastSeen = now;
                        break;
                    }
                }
            }
        }
    }
    
    // Update last priority tracking
    lastPriorityForCards = priority;
    
    // Step 1: Update existing slots - refresh timestamp if alert still exists
    for (int c = 0; c < 2; c++) {
        if (cards[c].lastSeen == 0) continue;
        
        bool stillExists = false;
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(cards[c].alert, alerts[i])) {
                    stillExists = true;
                    cards[c].alert = alerts[i];  // Update with latest data
                    cards[c].lastSeen = now;
                    break;
                }
            }
        }
        
        // Expire if past grace period
        if (!stillExists) {
            unsigned long age = now - cards[c].lastSeen;
            if (age > gracePeriodMs) {
                cards[c].alert = AlertData();
                cards[c].lastSeen = 0;
            }
        }
    }
    
    // Step 2: Add new non-priority alerts to empty slots
    // Skip priority alert - it's shown in the main display, not as a card
    if (alerts != nullptr) {
        for (int i = 0; i < alertCount; i++) {
            if (!alerts[i].isValid || alerts[i].band == BAND_NONE) continue;
            if (isSameAsPriority(alerts[i])) continue;  // Skip priority - don't waste a card slot
            
            // Check if already tracked
            bool found = false;
            for (int c = 0; c < 2; c++) {
                if (cards[c].lastSeen > 0 && alertsMatch(cards[c].alert, alerts[i])) {
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                // Find empty slot
                for (int c = 0; c < 2; c++) {
                    if (cards[c].lastSeen == 0) {
                        cards[c].alert = alerts[i];
                        cards[c].lastSeen = now;
                        break;
                    }
                }
            }
        }
    }
    
    // For debug logging if needed
    [[maybe_unused]] bool doDebug = false;
    
    // Helper: get signal bars for an alert based on direction
    auto getAlertBars = [](const AlertData& a) -> uint8_t {
        if (a.direction & DIR_FRONT) return a.frontStrength;
        if (a.direction & DIR_REAR) return a.rearStrength;
        return (a.frontStrength > a.rearStrength) ? a.frontStrength : a.rearStrength;
    };
    
    // Build list of cards to draw this frame (V1 alerts only)
    struct CardToDraw {
        int slot;           // V1 card slot index
        bool isGraced;
        uint8_t bars;       // Signal strength for V1 cards
    } cardsToDraw[2];
    int cardsToDrawCount = 0;
    
    // Add V1 secondary alerts
    for (int c = 0; c < 2 && cardsToDrawCount < 2; c++) {
        if (cards[c].lastSeen == 0) continue;
        if (isSameAsPriority(cards[c].alert)) continue;
        cardsToDraw[cardsToDrawCount].slot = c;
        cardsToDraw[cardsToDrawCount].bars = getAlertBars(cards[c].alert);
        // Check if live or graced
        bool isLive = false;
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(cards[c].alert, alerts[i])) {
                    isLive = true;
                    break;
                }
            }
        }
        cardsToDraw[cardsToDrawCount].isGraced = !isLive;
        cardsToDrawCount++;
    }
    
    // === INCREMENTAL UPDATE LOGIC ===
    // Instead of clearing all cards and redrawing, check each position independently
    
    // Capture dirty.cards before resetting (need it for redraw checks)
    bool doForceRedraw = dirty.cards;
    dirty.cards = false;  // Reset the force flag
    
    // Helper to check if position needs full redraw vs just update
    auto positionNeedsFullRedraw = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount) {
            // Position now empty but had content - needs clear
            return lastDrawnPositions[pos].band != BAND_NONE;
        }
        
        auto& last = lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];
        
        // V1 card - check if band/freq/direction changed (needs full card redraw)
        // Use frequency tolerance (±5 MHz) to handle V1 jitter
        const uint32_t FREQ_TOLERANCE_MHZ = 5;
        int slot = curr.slot;
        if (cards[slot].alert.band != last.band) return true;
        uint32_t freqDiff = (cards[slot].alert.frequency > last.frequency) 
            ? (cards[slot].alert.frequency - last.frequency) 
            : (last.frequency - cards[slot].alert.frequency);
        if (freqDiff > FREQ_TOLERANCE_MHZ) return true;
        if (cards[slot].alert.direction != last.direction) return true;
        if (curr.isGraced != last.isGraced) return true;
        if (muted != last.wasMuted) return true;
        return false;
    };
    
    // Helper to check if position needs dynamic update (bars only)
    auto positionNeedsDynamicUpdate = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount) return false;
        
        auto& last = lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];
        
        // V1 card - check signal bars
        if (curr.bars != last.bars) return true;
        return false;
    };
    
    [[maybe_unused]] const int signalBarsX = SCREEN_WIDTH - 200 - 2;
    
    // Process each card position
    for (int i = 0; i < 2; i++) {
        int cardX = startX + i * (cardW + cardSpacing);
        
        bool needsFullRedraw = positionNeedsFullRedraw(i) || doForceRedraw;
        bool needsDynamicUpdate = !needsFullRedraw && positionNeedsDynamicUpdate(i);
        
        // Clear position if it's now empty
        if (i >= cardsToDrawCount) {
            if (lastDrawnPositions[i].band != BAND_NONE) {
                FILL_RECT(cardX, cardY, cardW, cardH, PALETTE_BG);
                lastDrawnPositions[i].band = BAND_NONE;
                secondaryCardsRenderDirty_ = true;
            }
            continue;
        }
        
        if (!needsFullRedraw && !needsDynamicUpdate) {
            continue;  // Skip this position - nothing changed
        }
        secondaryCardsRenderDirty_ = true;
        
        // === V1 ALERT CARD ===
        int c = cardsToDraw[i].slot;
        const AlertData& alert = cards[c].alert;
        bool isGraced = cardsToDraw[i].isGraced;
        bool drawMuted = muted || isGraced;
        uint8_t bars = cardsToDraw[i].bars;
        
        // Card background and border colors
        uint16_t bandCol = getBandColor(alert.band);
        uint16_t bgCol, borderCol;
        
        if (isGraced) {
            bgCol = 0x2104;
            borderCol = PALETTE_MUTED;
        } else if (drawMuted) {
            bgCol = 0x2104;
            borderCol = PALETTE_MUTED;
        } else {
            uint8_t r = ((bandCol >> 11) & 0x1F) * 3 / 10;
            uint8_t g = ((bandCol >> 5) & 0x3F) * 3 / 10;
            uint8_t b = (bandCol & 0x1F) * 3 / 10;
            bgCol = (r << 11) | (g << 5) | b;
            borderCol = bandCol;
        }
        
        uint16_t contentCol = (isGraced || drawMuted) ? PALETTE_MUTED : TFT_WHITE;
        uint16_t bandLabelCol = (isGraced || drawMuted) ? PALETTE_MUTED : bandCol;
        
        if (needsFullRedraw) {
            // === FULL V1 CARD REDRAW ===
            FILL_ROUND_RECT(cardX, cardY, cardW, cardH, 5, bgCol);
            DRAW_ROUND_RECT(cardX, cardY, cardW, cardH, 5, borderCol);
            
            const int contentCenterY = cardY + 18;
            [[maybe_unused]] int topRowY = cardY + 11;
            
            // Direction arrow
            int arrowX = cardX + 18;
            int arrowCY = contentCenterY;
            if (alert.direction & DIR_FRONT) {
                tft->fillTriangle(arrowX, arrowCY - 7, arrowX - 6, arrowCY + 5, arrowX + 6, arrowCY + 5, contentCol);
            } else if (alert.direction & DIR_REAR) {
                tft->fillTriangle(arrowX, arrowCY + 7, arrowX - 6, arrowCY - 5, arrowX + 6, arrowCY - 5, contentCol);
            } else if (alert.direction & DIR_SIDE) {
                FILL_RECT(arrowX - 6, arrowCY - 2, 12, 4, contentCol);
            }
            
            // Band + frequency
            int labelX = cardX + 36;
            tft->setTextColor(bandLabelCol);
            tft->setTextSize(2);
            if (alert.band == BAND_LASER) {
                tft->setCursor(labelX, topRowY);
                tft->print("LASER");
            } else {
                const char* bandStr = bandToString(alert.band);
                tft->setCursor(labelX, topRowY);
                tft->print(bandStr);
                
                tft->setTextColor(contentCol);
                int freqX = labelX + strlen(bandStr) * 12 + 4;
                tft->setCursor(freqX, topRowY);
                if (alert.frequency > 0) {
                    char freqStr[10];
                    snprintf(freqStr, sizeof(freqStr), "%.3f", alert.frequency / 1000.0f);
                    tft->print(freqStr);
                } else {
                    tft->print("---");
                }
            }
            
            // Draw meter background
            const int meterY = cardY + 34;
            const int meterX = cardX + 10;
            const int meterW = cardW - 20;
            const int meterH = 18;
            FILL_RECT(meterX, meterY, meterW, meterH, 0x1082);
        }
        
        // Draw/update signal bars (always after full redraw, or on bars change)
        if (needsFullRedraw || needsDynamicUpdate) {
            const int meterY = cardY + 34;
            const int meterX = cardX + 10;
            const int meterW = cardW - 20;
            const int meterH = 18;
            const int barCount = 6;
            const int barSpacing = 2;
            const int barWidth = (meterW - (barCount - 1) * barSpacing) / barCount;
            
            // Clear meter area for bar update (not full redraw which already did it)
            if (!needsFullRedraw) {
                FILL_RECT(meterX, meterY, meterW, meterH, 0x1082);
            }
            
            uint16_t barColors[6] = {
                settings.colorBar1, settings.colorBar2, settings.colorBar3,
                settings.colorBar4, settings.colorBar5, settings.colorBar6
            };
            
            for (int b = 0; b < barCount; b++) {
                int barX = meterX + b * (barWidth + barSpacing);
                int barH = 10;
                int barY = meterY + (meterH - barH) / 2;
                
                if (b < bars) {
                    uint16_t fillColor = (isGraced || drawMuted) ? PALETTE_MUTED : barColors[b];
                    FILL_RECT(barX, barY, barWidth, barH, fillColor);
                } else {
                    DRAW_RECT(barX, barY, barWidth, barH, dimColor(barColors[b], 30));
                }
            }
        }
        
        // Update position tracking for V1 card
        lastDrawnPositions[i].band = alert.band;
        lastDrawnPositions[i].frequency = alert.frequency;
        lastDrawnPositions[i].direction = alert.direction;
        lastDrawnPositions[i].isGraced = isGraced;
        lastDrawnPositions[i].wasMuted = muted;
        lastDrawnPositions[i].bars = bars;
    }
    
    // Update global tracking
    lastDrawnCount = cardsToDrawCount;
#endif
}
