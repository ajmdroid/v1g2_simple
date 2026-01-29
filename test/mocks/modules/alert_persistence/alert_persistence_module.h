// Mock alert_persistence_module.h for native unit testing
#pragma once

// Forward declare AlertData (don't include packet_parser.h to avoid conflicts)
struct AlertData;

class AlertPersistenceModule {
public:
    // Test-controllable state - use simple inline AlertData for testing
    struct MockAlertData {
        int band = 0;
        int direction = 0;
        int frontStrength = 0;
        int rearStrength = 0;
        int frequency = 0;
        bool isValid = false;
        bool isPriority = false;
    };
    
    MockAlertData persistedAlert;
    unsigned long persistenceStartMs = 0;
    bool persistenceActive = false;
    
    // Call tracking
    int setPersistedAlertCalls = 0;
    int clearAllAlertStateCalls = 0;
    int clearPersistenceCalls = 0;
    int startPersistenceCalls = 0;
    int shouldShowPersistedCalls = 0;
    
    void reset() {
        persistedAlert = MockAlertData();
        persistenceStartMs = 0;
        persistenceActive = false;
        setPersistedAlertCalls = 0;
        clearAllAlertStateCalls = 0;
        clearPersistenceCalls = 0;
        startPersistenceCalls = 0;
        shouldShowPersistedCalls = 0;
    }
    
    // Template method accepts any AlertData-like struct
    template<typename T>
    void setPersistedAlert(const T& alert) {
        setPersistedAlertCalls++;
        persistedAlert.band = alert.band;
        persistedAlert.isValid = alert.isValid;
    }
    
    const MockAlertData& getPersistedAlert() const {
        return persistedAlert;
    }
    
    void clearAllAlertState() {
        clearAllAlertStateCalls++;
    }
    
    void clearPersistence() {
        clearPersistenceCalls++;
        persistenceActive = false;
    }
    
    void startPersistence(unsigned long nowMs) {
        startPersistenceCalls++;
        if (!persistenceActive) {
            persistenceStartMs = nowMs;
            persistenceActive = true;
        }
    }
    
    bool shouldShowPersisted(unsigned long nowMs, unsigned long durationMs) {
        shouldShowPersistedCalls++;
        if (!persistenceActive || !persistedAlert.isValid) return false;
        return (nowMs - persistenceStartMs) < durationMs;
    }
};
