#ifndef ERROR_CODES_H
#define ERROR_CODES_H

/**
 * @file error_codes.h
 * @brief Structured error codes for enterprise-grade error handling
 * 
 * Error code format: CATEGORY_SPECIFIC_ERROR
 * Categories:
 *   ERR_NONE (0)     - Success/no error
 *   ERR_BLE_*        - Bluetooth errors (100-199)
 *   ERR_GPS_*        - GPS errors (200-299)
 *   ERR_STORAGE_*    - Storage/filesystem errors (300-399)
 *   ERR_WIFI_*       - WiFi/network errors (400-499)
 *   ERR_V1_*         - V1 protocol errors (500-599)
 *   ERR_SYSTEM_*     - System/general errors (900-999)
 */

// ============================================================================
// SUCCESS
// ============================================================================
#define ERR_NONE                        0

// ============================================================================
// BLE ERRORS (100-199)
// ============================================================================
#define ERR_BLE_NOT_INITIALIZED         100
#define ERR_BLE_SCAN_FAILED             101
#define ERR_BLE_CONNECT_FAILED          102
#define ERR_BLE_CONNECT_TIMEOUT         103
#define ERR_BLE_DISCONNECT              104
#define ERR_BLE_SERVICE_NOT_FOUND       105
#define ERR_BLE_CHAR_NOT_FOUND          106
#define ERR_BLE_WRITE_FAILED            107
#define ERR_BLE_NOTIFY_FAILED           108
#define ERR_BLE_MTU_NEGOTIATION_FAILED  109
#define ERR_BLE_ALREADY_CONNECTED       110
#define ERR_BLE_DEVICE_NOT_FOUND        111
#define ERR_BLE_PAIRING_FAILED          112

// ============================================================================
// GPS ERRORS (200-299)
// ============================================================================
#define ERR_GPS_NOT_INITIALIZED         200
#define ERR_GPS_NO_FIX                  201
#define ERR_GPS_INVALID_DATA            202
#define ERR_GPS_TIMEOUT                 203
#define ERR_GPS_COMM_ERROR              204
#define ERR_GPS_CONFIG_FAILED           205
#define ERR_GPS_MODULE_NOT_DETECTED     206
#define ERR_GPS_BAUD_MISMATCH           207

// ============================================================================
// STORAGE ERRORS (300-399)
// ============================================================================
#define ERR_STORAGE_NOT_INITIALIZED     300
#define ERR_STORAGE_MOUNT_FAILED        301
#define ERR_STORAGE_FULL                302
#define ERR_STORAGE_READ_FAILED         303
#define ERR_STORAGE_WRITE_FAILED        304
#define ERR_STORAGE_FILE_NOT_FOUND      305
#define ERR_STORAGE_CORRUPT_DATA        306
#define ERR_STORAGE_PERMISSION_DENIED   307
#define ERR_STORAGE_SD_NOT_INSERTED     308
#define ERR_STORAGE_FORMAT_FAILED       309
#define ERR_STORAGE_JSON_PARSE_FAILED   310
#define ERR_STORAGE_JSON_TOO_LARGE      311

// ============================================================================
// WIFI ERRORS (400-499)
// ============================================================================
#define ERR_WIFI_NOT_INITIALIZED        400
#define ERR_WIFI_AP_START_FAILED        401
#define ERR_WIFI_STA_CONNECT_FAILED     402
#define ERR_WIFI_STA_TIMEOUT            403
#define ERR_WIFI_STA_WRONG_PASSWORD     404
#define ERR_WIFI_STA_NO_SSID            405
#define ERR_WIFI_SERVER_START_FAILED    406
#define ERR_WIFI_DNS_START_FAILED       407
#define ERR_WIFI_MDNS_START_FAILED      408
#define ERR_WIFI_OTA_FAILED             409
#define ERR_WIFI_HTTP_REQUEST_FAILED    410
#define ERR_WIFI_HTTP_TIMEOUT           411
#define ERR_WIFI_INVALID_URL            412

// ============================================================================
// V1 PROTOCOL ERRORS (500-599)
// ============================================================================
#define ERR_V1_INVALID_PACKET           500
#define ERR_V1_CHECKSUM_MISMATCH        501
#define ERR_V1_UNKNOWN_PACKET_TYPE      502
#define ERR_V1_MALFORMED_DATA           503
#define ERR_V1_QUEUE_FULL               504
#define ERR_V1_NOT_CONNECTED            505
#define ERR_V1_COMMAND_FAILED           506
#define ERR_V1_VERSION_MISMATCH         507

// ============================================================================
// SYSTEM ERRORS (900-999)
// ============================================================================
#define ERR_SYSTEM_OUT_OF_MEMORY        900
#define ERR_SYSTEM_TASK_CREATE_FAILED   901
#define ERR_SYSTEM_MUTEX_TIMEOUT        902
#define ERR_SYSTEM_INVALID_PARAMETER    903
#define ERR_SYSTEM_NOT_IMPLEMENTED      904
#define ERR_SYSTEM_HARDWARE_FAULT       905
#define ERR_SYSTEM_WATCHDOG_TIMEOUT     906
#define ERR_SYSTEM_LOW_BATTERY          907

// ============================================================================
// HELPER MACROS
// ============================================================================

/**
 * @brief Get error category from error code
 * @param err Error code
 * @return Category base (100, 200, etc.) or 0 for success
 */
#define ERR_CATEGORY(err)       ((err) / 100 * 100)

/**
 * @brief Check if error code indicates BLE error
 */
#define IS_BLE_ERROR(err)       ((err) >= 100 && (err) < 200)

/**
 * @brief Check if error code indicates GPS error
 */
#define IS_GPS_ERROR(err)       ((err) >= 200 && (err) < 300)

/**
 * @brief Check if error code indicates storage error
 */
#define IS_STORAGE_ERROR(err)   ((err) >= 300 && (err) < 400)

/**
 * @brief Check if error code indicates WiFi error
 */
#define IS_WIFI_ERROR(err)      ((err) >= 400 && (err) < 500)

/**
 * @brief Check if error code indicates V1 protocol error
 */
#define IS_V1_ERROR(err)        ((err) >= 500 && (err) < 600)

/**
 * @brief Check if error code indicates system error
 */
#define IS_SYSTEM_ERROR(err)    ((err) >= 900 && (err) < 1000)

/**
 * @brief Get human-readable error string (for debugging)
 * @param err Error code
 * @return Static string describing the error
 */
inline const char* errToString(int err) {
    switch (err) {
        case ERR_NONE:                      return "Success";
        
        // BLE
        case ERR_BLE_NOT_INITIALIZED:       return "BLE not initialized";
        case ERR_BLE_SCAN_FAILED:           return "BLE scan failed";
        case ERR_BLE_CONNECT_FAILED:        return "BLE connection failed";
        case ERR_BLE_CONNECT_TIMEOUT:       return "BLE connection timeout";
        case ERR_BLE_DISCONNECT:            return "BLE disconnected";
        case ERR_BLE_SERVICE_NOT_FOUND:     return "BLE service not found";
        case ERR_BLE_CHAR_NOT_FOUND:        return "BLE characteristic not found";
        case ERR_BLE_WRITE_FAILED:          return "BLE write failed";
        case ERR_BLE_NOTIFY_FAILED:         return "BLE notification failed";
        case ERR_BLE_MTU_NEGOTIATION_FAILED: return "BLE MTU negotiation failed";
        case ERR_BLE_ALREADY_CONNECTED:     return "BLE already connected";
        case ERR_BLE_DEVICE_NOT_FOUND:      return "BLE device not found";
        case ERR_BLE_PAIRING_FAILED:        return "BLE pairing failed";
        
        // GPS
        case ERR_GPS_NOT_INITIALIZED:       return "GPS not initialized";
        case ERR_GPS_NO_FIX:                return "GPS no fix";
        case ERR_GPS_INVALID_DATA:          return "GPS invalid data";
        case ERR_GPS_TIMEOUT:               return "GPS timeout";
        case ERR_GPS_COMM_ERROR:            return "GPS communication error";
        case ERR_GPS_CONFIG_FAILED:         return "GPS configuration failed";
        case ERR_GPS_MODULE_NOT_DETECTED:   return "GPS module not detected";
        case ERR_GPS_BAUD_MISMATCH:         return "GPS baud rate mismatch";
        
        // Storage
        case ERR_STORAGE_NOT_INITIALIZED:   return "Storage not initialized";
        case ERR_STORAGE_MOUNT_FAILED:      return "Storage mount failed";
        case ERR_STORAGE_FULL:              return "Storage full";
        case ERR_STORAGE_READ_FAILED:       return "Storage read failed";
        case ERR_STORAGE_WRITE_FAILED:      return "Storage write failed";
        case ERR_STORAGE_FILE_NOT_FOUND:    return "File not found";
        case ERR_STORAGE_CORRUPT_DATA:      return "Corrupt data";
        case ERR_STORAGE_PERMISSION_DENIED: return "Permission denied";
        case ERR_STORAGE_SD_NOT_INSERTED:   return "SD card not inserted";
        case ERR_STORAGE_FORMAT_FAILED:     return "Storage format failed";
        case ERR_STORAGE_JSON_PARSE_FAILED: return "JSON parse failed";
        case ERR_STORAGE_JSON_TOO_LARGE:    return "JSON too large";
        
        // WiFi
        case ERR_WIFI_NOT_INITIALIZED:      return "WiFi not initialized";
        case ERR_WIFI_AP_START_FAILED:      return "WiFi AP start failed";
        case ERR_WIFI_STA_CONNECT_FAILED:   return "WiFi station connect failed";
        case ERR_WIFI_STA_TIMEOUT:          return "WiFi connection timeout";
        case ERR_WIFI_STA_WRONG_PASSWORD:   return "WiFi wrong password";
        case ERR_WIFI_STA_NO_SSID:          return "WiFi SSID not found";
        case ERR_WIFI_SERVER_START_FAILED:  return "WiFi server start failed";
        case ERR_WIFI_DNS_START_FAILED:     return "DNS server start failed";
        case ERR_WIFI_MDNS_START_FAILED:    return "mDNS start failed";
        case ERR_WIFI_OTA_FAILED:           return "OTA update failed";
        case ERR_WIFI_HTTP_REQUEST_FAILED:  return "HTTP request failed";
        case ERR_WIFI_HTTP_TIMEOUT:         return "HTTP timeout";
        case ERR_WIFI_INVALID_URL:          return "Invalid URL";
        
        // V1 Protocol
        case ERR_V1_INVALID_PACKET:         return "Invalid V1 packet";
        case ERR_V1_CHECKSUM_MISMATCH:      return "V1 checksum mismatch";
        case ERR_V1_UNKNOWN_PACKET_TYPE:    return "Unknown V1 packet type";
        case ERR_V1_MALFORMED_DATA:         return "Malformed V1 data";
        case ERR_V1_QUEUE_FULL:             return "V1 queue full";
        case ERR_V1_NOT_CONNECTED:          return "V1 not connected";
        case ERR_V1_COMMAND_FAILED:         return "V1 command failed";
        case ERR_V1_VERSION_MISMATCH:       return "V1 version mismatch";
        
        // System
        case ERR_SYSTEM_OUT_OF_MEMORY:      return "Out of memory";
        case ERR_SYSTEM_TASK_CREATE_FAILED: return "Task creation failed";
        case ERR_SYSTEM_MUTEX_TIMEOUT:      return "Mutex timeout";
        case ERR_SYSTEM_INVALID_PARAMETER:  return "Invalid parameter";
        case ERR_SYSTEM_NOT_IMPLEMENTED:    return "Not implemented";
        case ERR_SYSTEM_HARDWARE_FAULT:     return "Hardware fault";
        case ERR_SYSTEM_WATCHDOG_TIMEOUT:   return "Watchdog timeout";
        case ERR_SYSTEM_LOW_BATTERY:        return "Low battery";
        
        default:                            return "Unknown error";
    }
}

#endif // ERROR_CODES_H
