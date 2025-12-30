#ifndef SERIAL_LOGGER_H
#define SERIAL_LOGGER_H

#include <Arduino.h>
#include <FS.h>
#include "alert_logger.h"

// Serial logger that writes to both Serial and SD card
// Usage: Replace Serial.print/println with SerialLog.print/println

#ifndef SERIAL_LOG_PATH
#define SERIAL_LOG_PATH "/serial_log.txt"
#endif

#ifndef SERIAL_LOG_MAX_SIZE
#define SERIAL_LOG_MAX_SIZE (2UL * 1024 * 1024 * 1024)  // 2GB max, then rotate
#endif

class SerialLogger : public Print {
public:
    SerialLogger() : fs(nullptr), enabled(false), fileSize(0) {}
    
    // Initialize with filesystem from AlertLogger
    void begin() {
        fs = alertLogger.getFilesystem();
        if (fs) {
            enabled = true;
            
            // Check existing file size
            if (fs->exists(SERIAL_LOG_PATH)) {
                File f = fs->open(SERIAL_LOG_PATH, FILE_READ);
                if (f) {
                    fileSize = f.size();
                    f.close();
                }
            }
            
            // Write startup marker
            File file = fs->open(SERIAL_LOG_PATH, FILE_APPEND);
            if (file) {
                file.println("\n\n========== BOOT ==========");
                file.printf("Timestamp: %lu ms\n", millis());
                file.println("==========================\n");
                file.close();
            }
        }
    }
    
    // Print interface implementation
    virtual size_t write(uint8_t c) override {
        // Always write to hardware Serial
        Serial.write(c);
        
        // Also write to SD if enabled
        if (enabled && fs) {
            File file = fs->open(SERIAL_LOG_PATH, FILE_APPEND);
            if (file) {
                file.write(c);
                fileSize++;
                file.close();
                
                // Rotate if too large
                if (fileSize > SERIAL_LOG_MAX_SIZE) {
                    rotate();
                }
            }
        }
        return 1;
    }
    
    virtual size_t write(const uint8_t *buffer, size_t size) override {
        // Always write to hardware Serial
        Serial.write(buffer, size);
        
        // Also write to SD if enabled
        if (enabled && fs) {
            File file = fs->open(SERIAL_LOG_PATH, FILE_APPEND);
            if (file) {
                size_t written = file.write(buffer, size);
                fileSize += written;
                file.close();
                
                // Rotate if too large
                if (fileSize > SERIAL_LOG_MAX_SIZE) {
                    rotate();
                }
                return written;
            }
        }
        return size;
    }
    
    // Enable/disable SD logging
    void setEnabled(bool en) { enabled = en && (fs != nullptr); }
    bool isEnabled() const { return enabled; }
    
    // Get current log size
    size_t getLogSize() const { return fileSize; }
    
    // Clear the log file
    bool clear() {
        if (!fs) return false;
        
        if (fs->exists(SERIAL_LOG_PATH)) {
            fs->remove(SERIAL_LOG_PATH);
        }
        fileSize = 0;
        return true;
    }
    
    // Rotate log (rename old, start new)
    void rotate() {
        if (!fs) return;
        
        // Remove old backup if exists
        if (fs->exists("/serial_log_old.txt")) {
            fs->remove("/serial_log_old.txt");
        }
        
        // Rename current to old
        if (fs->exists(SERIAL_LOG_PATH)) {
            fs->rename(SERIAL_LOG_PATH, "/serial_log_old.txt");
        }
        
        fileSize = 0;
    }

private:
    fs::FS* fs;
    bool enabled;
    size_t fileSize;
};

extern SerialLogger SerialLog;

#endif // SERIAL_LOGGER_H
