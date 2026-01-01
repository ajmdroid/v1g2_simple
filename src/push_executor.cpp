/**
 * Push Executor - Transactional Auto-Push Implementation
 */

#include "push_executor.h"
#include "ble_client.h"
#include "v1_profiles.h"
#include "settings.h"
#include "storage_manager.h"
#include <FS.h>

// Global instance
PushExecutor pushExecutor;

PushExecutor::PushExecutor() {
    plan.reset();
}

void PushExecutor::begin(V1BLEClient* ble, V1ProfileManager* profiles, SettingsManager* settings) {
    bleClient = ble;
    profileManager = profiles;
    settingsManager = settings;
}

int PushExecutor::resolveDeviceSlot(const String& v1Address) {
    // Check for device-specific mapping in known_v1_profiles.txt
    if (!storageManager.isReady()) {
        return -1;
    }
    
    fs::FS* fs = storageManager.getFilesystem();
    File profileFile = fs->open("/known_v1_profiles.txt", FILE_READ);
    if (!profileFile) {
        return -1;
    }
    
    int deviceSlot = -1;
    while (profileFile.available()) {
        String line = profileFile.readStringUntil('\n');
        line.trim();
        int sep = line.indexOf('|');
        if (sep > 0) {
            String addr = line.substring(0, sep);
            if (addr.equalsIgnoreCase(v1Address)) {
                deviceSlot = line.substring(sep + 1).toInt();
                Serial.printf("[PushExec] Device mapping: %s → slot %d\n", v1Address.c_str(), deviceSlot);
                break;
            }
        }
    }
    profileFile.close();
    
    return deviceSlot;
}

bool PushExecutor::buildPlan(int slotIndex) {
    if (!settingsManager || !profileManager) {
        plan.failReason = "Not initialized";
        return false;
    }
    
    // Clamp slot index
    slotIndex = constrain(slotIndex, 0, 2);
    plan.resolvedSlot = slotIndex;
    
    // Get slot configuration
    AutoPushSlot slot = settingsManager->getSlot(slotIndex);
    
    plan.commandCount = 0;
    
    // Command 1: User bytes (profile settings)
    if (slot.profileName.length() > 0) {
        V1Profile profile;
        if (profileManager->loadProfile(slot.profileName, profile)) {
            PushCommand& cmd = plan.commands[plan.commandCount++];
            cmd.type = PUSH_CMD_USER_BYTES;
            cmd.status = CMD_STATUS_PENDING;
            memcpy(cmd.data.userBytes.bytes, profile.settings.bytes, 6);
            
            // Command 2: Display on/off (from profile)
            PushCommand& dispCmd = plan.commands[plan.commandCount++];
            dispCmd.type = PUSH_CMD_DISPLAY;
            dispCmd.status = CMD_STATUS_PENDING;
            dispCmd.data.display.on = profile.displayOn;
            
            Serial.printf("[PushExec] Plan: profile='%s' display=%s\n", 
                           slot.profileName.c_str(), profile.displayOn ? "ON" : "OFF");
        } else {
            Serial.printf("[PushExec] WARNING: Could not load profile '%s'\n", slot.profileName.c_str());
        }
    }
    
    // Command 3: Mode
    if (slot.mode != V1_MODE_UNKNOWN) {
        PushCommand& cmd = plan.commands[plan.commandCount++];
        cmd.type = PUSH_CMD_MODE;
        cmd.status = CMD_STATUS_PENDING;
        cmd.data.modeCmd.mode = static_cast<uint8_t>(slot.mode);
        
        const char* modeName = "Unknown";
        if (slot.mode == V1_MODE_ALL_BOGEYS) modeName = "All Bogeys";
        else if (slot.mode == V1_MODE_LOGIC) modeName = "Logic";
        else if (slot.mode == V1_MODE_ADVANCED_LOGIC) modeName = "Advanced Logic";
        Serial.printf("[PushExec] Plan: mode=%s\n", modeName);
    }
    
    // Command 4: Volume (only if BOTH are set)
    uint8_t mainVol = settingsManager->getSlotVolume(slotIndex);
    uint8_t muteVol = settingsManager->getSlotMuteVolume(slotIndex);
    if (mainVol != 0xFF && muteVol != 0xFF) {
        PushCommand& cmd = plan.commands[plan.commandCount++];
        cmd.type = PUSH_CMD_VOLUME;
        cmd.status = CMD_STATUS_PENDING;
        cmd.data.volume.main = mainVol;
        cmd.data.volume.muted = muteVol;
        Serial.printf("[PushExec] Plan: volume=%d/%d\n", mainVol, muteVol);
    }
    
    if (plan.commandCount == 0) {
        plan.failReason = "No commands to execute (empty slot)";
        return false;
    }
    
    Serial.printf("[PushExec] Built plan with %d commands\n", plan.commandCount);
    return true;
}

bool PushExecutor::startPush(const String& v1Address, int slotOverride) {
    if (plan.state != PUSH_STATE_IDLE) {
        Serial.println("[PushExec] Push already in progress, rejecting new push");
        return false;
    }
    
    if (!bleClient || !bleClient->isConnected()) {
        Serial.println("[PushExec] Cannot start push - not connected");
        return false;
    }
    
    plan.reset();
    plan.state = PUSH_STATE_RESOLVING;
    plan.targetV1Address = v1Address;
    plan.startedAtMs = millis();
    plan.lastActivityMs = plan.startedAtMs;
    
    metrics.totalPushes++;
    
    Serial.printf("[PushExec] Starting push for %s\n", v1Address.c_str());
    
    // Resolve device slot
    int slot = slotOverride;
    if (slot < 0) {
        slot = resolveDeviceSlot(v1Address);
    }
    if (slot < 0) {
        // Fall back to global active slot
        slot = settingsManager->get().activeSlot;
    }
    
    Serial.printf("[PushExec] Resolved slot: %d\n", slot);
    
    // Build command plan
    plan.state = PUSH_STATE_PLANNING;
    if (!buildPlan(slot)) {
        finishPush(PUSH_RESULT_FAILED, plan.failReason);
        return false;
    }
    
    // Start execution
    plan.state = PUSH_STATE_EXECUTING;
    plan.currentCommandIndex = 0;
    
    return true;
}

void PushExecutor::cancelPush(const String& reason) {
    if (plan.state == PUSH_STATE_IDLE) {
        return;
    }
    Serial.printf("[PushExec] Cancelled: %s\n", reason.c_str());
    finishPush(PUSH_RESULT_FAILED, reason);
}

bool PushExecutor::executeCurrentCommand() {
    if (plan.currentCommandIndex >= plan.commandCount) {
        return false;
    }
    
    PushCommand& cmd = plan.commands[plan.currentCommandIndex];
    
    if (cmd.status != CMD_STATUS_PENDING && cmd.status != CMD_STATUS_SENT) {
        return false;
    }
    
    bool sent = false;
    
    switch (cmd.type) {
        case PUSH_CMD_USER_BYTES:
            Serial.printf("[PushExec] Sending user bytes: %02X%02X%02X%02X%02X%02X\n",
                           cmd.data.userBytes.bytes[0], cmd.data.userBytes.bytes[1],
                           cmd.data.userBytes.bytes[2], cmd.data.userBytes.bytes[3],
                           cmd.data.userBytes.bytes[4], cmd.data.userBytes.bytes[5]);
            sent = bleClient->writeUserBytes(cmd.data.userBytes.bytes);
            break;
            
        case PUSH_CMD_DISPLAY:
            Serial.printf("[PushExec] Sending display: %s\n", cmd.data.display.on ? "ON" : "OFF");
            sent = bleClient->setDisplayOn(cmd.data.display.on);
            break;
            
        case PUSH_CMD_MODE:
            Serial.printf("[PushExec] Sending mode: %d\n", cmd.data.modeCmd.mode);
            sent = bleClient->setMode(cmd.data.modeCmd.mode);
            break;
            
        case PUSH_CMD_VOLUME:
            Serial.printf("[PushExec] Sending volume: %d/%d\n", 
                           cmd.data.volume.main, cmd.data.volume.muted);
            sent = bleClient->setVolume(cmd.data.volume.main, cmd.data.volume.muted);
            break;
            
        default:
            Serial.println("[PushExec] Unknown command type");
            cmd.status = CMD_STATUS_SKIPPED;
            return false;
    }
    
    if (sent) {
        cmd.status = CMD_STATUS_SENT;
        cmd.sentAtMs = millis();
        metrics.cmdsSent++;
        plan.lastActivityMs = cmd.sentAtMs;
        Serial.printf("[PushExec] Command %d sent (verification disabled for user bytes)\n", 
                       plan.currentCommandIndex);
    } else {
        Serial.printf("[PushExec] Command %d send failed\n", plan.currentCommandIndex);
        cmd.retryCount++;
        metrics.totalRetries++;
        
        if (cmd.retryCount > MAX_RETRIES) {
            cmd.status = CMD_STATUS_FAILED;
            metrics.cmdsFailed++;
            return false;
        }
    }
    
    return sent;
}

bool PushExecutor::checkVerification() {
    if (plan.currentCommandIndex >= plan.commandCount) {
        return true;  // All done
    }
    
    PushCommand& cmd = plan.commands[plan.currentCommandIndex];
    
    if (cmd.status != CMD_STATUS_SENT) {
        return false;
    }
    
    bool verified = false;
    
    switch (cmd.type) {
        case PUSH_CMD_USER_BYTES:
            // Verification disabled for user bytes; assume success
            verified = true;
            plan.userBytesReceived = false;
            Serial.println("[PushExec] User bytes assumed OK (verification disabled)");
            break;
            
        case PUSH_CMD_DISPLAY:
            // Display doesn't have readback - trust it after send
            verified = true;
            Serial.println("[PushExec] Display command assumed OK (no readback)");
            break;
            
        case PUSH_CMD_MODE:
            // Mode can be verified from status packets, but for now trust it
            // TODO: Check parser mode if available
            verified = true;
            Serial.println("[PushExec] Mode command assumed OK");
            break;
            
        case PUSH_CMD_VOLUME:
            // Volume doesn't have readback - trust it after send
            verified = true;
            Serial.println("[PushExec] Volume command assumed OK (no readback)");
            break;
            
        default:
            verified = true;
            break;
    }
    
    return verified;
}

void PushExecutor::completeCurrentCommand(bool success) {
    if (plan.currentCommandIndex >= plan.commandCount) {
        return;
    }
    
    PushCommand& cmd = plan.commands[plan.currentCommandIndex];
    
    if (success) {
        cmd.status = CMD_STATUS_VERIFIED;
        cmd.verifiedAtMs = millis();
        metrics.cmdsVerified++;
        Serial.printf("[PushExec] Command %d verified\n", plan.currentCommandIndex);
    } else {
        cmd.status = CMD_STATUS_FAILED;
        metrics.cmdsFailed++;
        Serial.printf("[PushExec] Command %d failed\n", plan.currentCommandIndex);
    }
    
    plan.currentCommandIndex++;
    plan.lastActivityMs = millis();
}

void PushExecutor::finishPush(PushResult result, const String& reason) {
    plan.result = result;
    plan.failReason = reason;
    
    uint32_t duration = millis() - plan.startedAtMs;
    metrics.lastPushDurationMs = duration;
    
    // Update running average
    if (metrics.avgPushDurationMs == 0) {
        metrics.avgPushDurationMs = duration;
    } else {
        metrics.avgPushDurationMs = (metrics.avgPushDurationMs * 7 + duration) / 8;
    }
    
    switch (result) {
        case PUSH_RESULT_SUCCESS:
            metrics.successCount++;
            Serial.printf("[PushExec] SUCCESS in %lu ms\n", duration);
            break;
        case PUSH_RESULT_PARTIAL:
            metrics.partialCount++;
            Serial.printf("[PushExec] PARTIAL: %s (%lu ms)\n", reason.c_str(), duration);
            break;
        case PUSH_RESULT_TIMEOUT:
            metrics.timeoutCount++;
            metrics.lastFailReason = reason;
            Serial.printf("[PushExec] TIMEOUT: %s (%lu ms)\n", reason.c_str(), duration);
            break;
        case PUSH_RESULT_DISCONNECTED:
            metrics.disconnectCount++;
            metrics.lastFailReason = reason;
            Serial.printf("[PushExec] DISCONNECTED: %s (%lu ms)\n", reason.c_str(), duration);
            break;
        case PUSH_RESULT_FAILED:
        default:
            metrics.failCount++;
            metrics.lastFailReason = reason;
            Serial.printf("[PushExec] FAILED: %s (%lu ms)\n", reason.c_str(), duration);
            break;
    }
    
    plan.state = PUSH_STATE_IDLE;
}

bool PushExecutor::process(uint32_t latencyMs) {
    if (plan.state == PUSH_STATE_IDLE) {
        return false;
    }
    
    // Check for disconnection
    if (!bleClient || !bleClient->isConnected()) {
        finishPush(PUSH_RESULT_DISCONNECTED, "V1 disconnected");
        return false;
    }
    
    // Check total timeout
    uint32_t elapsed = millis() - plan.startedAtMs;
    if (elapsed > TOTAL_TIMEOUT_MS) {
        finishPush(PUSH_RESULT_TIMEOUT, "Total timeout exceeded");
        return false;
    }
    
    // Backpressure: if BLE→display latency is high, pause TX
    if (latencyMs > BACKPRESSURE_THRESHOLD_MS) {
        metrics.backpressurePauses++;
        return false;  // Don't make progress this cycle
    }
    
    bool madeProgress = false;
    
    switch (plan.state) {
        case PUSH_STATE_EXECUTING: {
            // Check if current command needs to be sent
            if (plan.currentCommandIndex < plan.commandCount) {
                PushCommand& cmd = plan.commands[plan.currentCommandIndex];
                
                if (cmd.status == CMD_STATUS_PENDING) {
                    // Send the command
                    if (executeCurrentCommand()) {
                        plan.state = PUSH_STATE_VERIFYING;
                        madeProgress = true;
                    } else if (cmd.status == CMD_STATUS_FAILED) {
                        // Command failed after retries
                        // Continue to next command (partial success)
                        completeCurrentCommand(false);
                        madeProgress = true;
                    }
                }
            } else {
                // All commands processed
                int verified = 0, failed = 0;
                for (int i = 0; i < plan.commandCount; i++) {
                    if (plan.commands[i].status == CMD_STATUS_VERIFIED) verified++;
                    if (plan.commands[i].status == CMD_STATUS_FAILED) failed++;
                }
                
                if (failed == 0) {
                    finishPush(PUSH_RESULT_SUCCESS);
                } else if (verified > 0) {
                    finishPush(PUSH_RESULT_PARTIAL, 
                              String(failed) + " of " + String(plan.commandCount) + " commands failed");
                } else {
                    finishPush(PUSH_RESULT_FAILED, "All commands failed");
                }
                madeProgress = true;
            }
            break;
        }
        
        case PUSH_STATE_VERIFYING: {
            PushCommand& cmd = plan.commands[plan.currentCommandIndex];
            
            // Check verification
            if (checkVerification()) {
                completeCurrentCommand(true);
                plan.state = PUSH_STATE_EXECUTING;
                
                // Add inter-command delay
                if (plan.currentCommandIndex < plan.commandCount) {
                    delay(INTER_CMD_DELAY_MS);
                }
                madeProgress = true;
            } else {
                // Check per-command timeout
                uint32_t cmdElapsed = millis() - cmd.sentAtMs;
                if (cmdElapsed > VERIFY_TIMEOUT_MS) {
                    // Timeout waiting for verification
                    cmd.retryCount++;
                    metrics.totalRetries++;
                    
                    if (cmd.retryCount > MAX_RETRIES) {
                        Serial.printf("[PushExec] Cmd %d verification timeout after %d retries\n",
                                       plan.currentCommandIndex, cmd.retryCount);
                        completeCurrentCommand(false);
                        plan.state = PUSH_STATE_EXECUTING;
                    } else {
                        // Retry the command
                        Serial.printf("[PushExec] Cmd %d verification timeout, retry %d\n",
                                       plan.currentCommandIndex, cmd.retryCount);
                        cmd.status = CMD_STATUS_PENDING;
                        plan.state = PUSH_STATE_EXECUTING;
                    }
                    madeProgress = true;
                }
            }
            break;
        }
        
        default:
            break;
    }
    
    return madeProgress;
}

void PushExecutor::onUserBytesReceived(const uint8_t* bytes) {
    if (bytes) {
        memcpy(plan.receivedUserBytes, bytes, 6);
        plan.userBytesReceived = true;
        Serial.printf("[PushExec] Received user bytes: %02X%02X%02X%02X%02X%02X\n",
                       bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    }
}

void PushExecutor::onModeReceived(uint8_t mode) {
    plan.modeReceived = true;
    plan.receivedMode = mode;
}
