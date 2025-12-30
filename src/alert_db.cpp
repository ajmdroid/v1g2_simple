/**
 * SQLite Alert Database Implementation
 */

#include "alert_db.h"
#include <SD_MMC.h>

AlertDB alertDB;

AlertDB::AlertDB() 
    : db(nullptr)
    , sessionId(0)
    , hasGPS(false)
    , gpsLat(0)
    , gpsLon(0)
    , gpsSpeed(0)
    , gpsHeading(0)
    , hasRTC(false)
    , rtcTimestamp(0)
{
    lastAlert = LastAlert();
}

AlertDB::~AlertDB() {
    end();
}

bool AlertDB::begin() {
    if (db) {
        return true;  // Already open
    }
    
    Serial.println("[AlertDB] Initializing SQLite database...");
    
    // Initialize SQLite
    sqlite3_initialize();
    
    // Open database (creates if doesn't exist)
    int rc = sqlite3_open(ALERT_DB_PATH, &db);
    if (rc != SQLITE_OK) {
        Serial.printf("[AlertDB] ERROR: Failed to open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        db = nullptr;
        return false;
    }
    
    Serial.printf("[AlertDB] Database opened: %s\n", ALERT_DB_PATH);
    
    // Create schema if needed
    if (!createSchema()) {
        Serial.println("[AlertDB] ERROR: Failed to create schema");
        end();
        return false;
    }
    
    // Initialize new session
    if (!initSession()) {
        Serial.println("[AlertDB] ERROR: Failed to init session");
        end();
        return false;
    }
    
    Serial.printf("[AlertDB] Ready - Session ID: %u\n", sessionId);
    return true;
}

void AlertDB::end() {
    if (db) {
        sqlite3_close(db);
        db = nullptr;
        Serial.println("[AlertDB] Database closed");
    }
}

bool AlertDB::createSchema() {
    // Create alerts table
    const char* createAlerts = R"(
        CREATE TABLE IF NOT EXISTS alerts (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_ms INTEGER NOT NULL,
            timestamp_utc INTEGER,
            session_id INTEGER NOT NULL,
            band TEXT,
            frequency INTEGER,
            direction TEXT,
            strength_front INTEGER,
            strength_rear INTEGER,
            alert_count INTEGER,
            muted INTEGER,
            latitude REAL,
            longitude REAL,
            speed_mph REAL,
            heading REAL,
            event TEXT NOT NULL
        );
    )";
    
    if (!execSQL(createAlerts)) {
        return false;
    }
    
    // Create sessions table to track power cycles
    const char* createSessions = R"(
        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            start_ms INTEGER NOT NULL,
            start_utc INTEGER,
            alerts_count INTEGER DEFAULT 0
        );
    )";
    
    if (!execSQL(createSessions)) {
        return false;
    }
    
    // Create indexes for common queries
    execSQL("CREATE INDEX IF NOT EXISTS idx_alerts_timestamp ON alerts(timestamp_ms);");
    execSQL("CREATE INDEX IF NOT EXISTS idx_alerts_session ON alerts(session_id);");
    execSQL("CREATE INDEX IF NOT EXISTS idx_alerts_band ON alerts(band);");
    execSQL("CREATE INDEX IF NOT EXISTS idx_alerts_freq ON alerts(frequency);");
    execSQL("CREATE INDEX IF NOT EXISTS idx_alerts_location ON alerts(latitude, longitude);");
    
    return true;
}

bool AlertDB::initSession() {
    // Create new session record
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT INTO sessions (start_ms, start_utc) VALUES (%lu, %s);",
        millis(),
        hasRTC ? String(rtcTimestamp).c_str() : "NULL"
    );
    
    if (!execSQL(sql)) {
        return false;
    }
    
    // Get the session ID
    sessionId = (uint32_t)sqlite3_last_insert_rowid(db);
    return true;
}

bool AlertDB::execSQL(const char* sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    
    if (rc != SQLITE_OK) {
        Serial.printf("[AlertDB] SQL error: %s\n", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

String AlertDB::statusText() const {
    if (!db) {
        return "DB not initialized";
    }
    return "SQLite ready (session " + String(sessionId) + ")";
}

bool AlertDB::shouldLog(const AlertData& alert, const DisplayState& state, size_t count) {
    bool isActive = alert.isValid && alert.band != BAND_NONE;
    
    // Always log transitions
    if (isActive != lastAlert.active) {
        return true;
    }
    
    // If both inactive, don't log again
    if (!isActive && !lastAlert.active) {
        return false;
    }
    
    // Log if any field changed
    return alert.band != lastAlert.band ||
           alert.direction != lastAlert.direction ||
           alert.frequency != lastAlert.frequency ||
           alert.frontStrength != lastAlert.front ||
           alert.rearStrength != lastAlert.rear ||
           count != lastAlert.count ||
           state.muted != lastAlert.muted;
}

String AlertDB::bandToString(Band band) const {
    switch (band) {
        case BAND_KA: return "Ka";
        case BAND_K:  return "K";
        case BAND_X:  return "X";
        case BAND_LASER: return "LASER";
        default: return "NONE";
    }
}

String AlertDB::dirToString(Direction dir) const {
    switch (dir) {
        case DIR_FRONT: return "FRONT";
        case DIR_SIDE:  return "SIDE";
        case DIR_REAR:  return "REAR";
        default: return "NONE";
    }
}

bool AlertDB::logAlert(const AlertData& alert, const DisplayState& state, size_t alertCount) {
    if (!db) return false;
    
    if (!shouldLog(alert, state, alertCount)) {
        return true;  // Skip duplicate
    }
    
    // Update last alert state
    lastAlert.active = alert.isValid && alert.band != BAND_NONE;
    lastAlert.band = alert.band;
    lastAlert.direction = alert.direction;
    lastAlert.frequency = alert.frequency;
    lastAlert.front = alert.frontStrength;
    lastAlert.rear = alert.rearStrength;
    lastAlert.count = alertCount;
    lastAlert.muted = state.muted;
    
    // Determine event type
    const char* event = lastAlert.active ? "ALERT" : "CLEAR";
    
    // Build SQL
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO alerts (timestamp_ms, timestamp_utc, session_id, band, frequency, "
        "direction, strength_front, strength_rear, alert_count, muted, "
        "latitude, longitude, speed_mph, heading, event) "
        "VALUES (%lu, %s, %u, '%s', %u, '%s', %u, %u, %zu, %d, %s, %s, %s, %s, '%s');",
        millis(),
        hasRTC ? String(rtcTimestamp).c_str() : "NULL",
        sessionId,
        bandToString(alert.band).c_str(),
        alert.frequency,
        dirToString(alert.direction).c_str(),
        alert.frontStrength,
        alert.rearStrength,
        alertCount,
        state.muted ? 1 : 0,
        hasGPS ? String(gpsLat, 6).c_str() : "NULL",
        hasGPS ? String(gpsLon, 6).c_str() : "NULL",
        hasGPS ? String(gpsSpeed, 1).c_str() : "NULL",
        hasGPS ? String(gpsHeading, 1).c_str() : "NULL",
        event
    );
    
    if (!execSQL(sql)) {
        return false;
    }
    
    // Update session alert count
    char updateSql[128];
    snprintf(updateSql, sizeof(updateSql),
        "UPDATE sessions SET alerts_count = alerts_count + 1 WHERE id = %u;",
        sessionId
    );
    execSQL(updateSql);
    
    return true;
}

bool AlertDB::logClear() {
    if (!db) return false;
    
    // Only log if we were active
    if (!lastAlert.active) {
        return true;
    }
    
    lastAlert.active = false;
    
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO alerts (timestamp_ms, timestamp_utc, session_id, band, frequency, "
        "direction, strength_front, strength_rear, alert_count, muted, "
        "latitude, longitude, speed_mph, heading, event) "
        "VALUES (%lu, %s, %u, NULL, NULL, NULL, NULL, NULL, 0, 0, %s, %s, %s, %s, 'CLEAR');",
        millis(),
        hasRTC ? String(rtcTimestamp).c_str() : "NULL",
        sessionId,
        hasGPS ? String(gpsLat, 6).c_str() : "NULL",
        hasGPS ? String(gpsLon, 6).c_str() : "NULL",
        hasGPS ? String(gpsSpeed, 1).c_str() : "NULL",
        hasGPS ? String(gpsHeading, 1).c_str() : "NULL"
    );
    
    return execSQL(sql);
}

void AlertDB::setGPS(double lat, double lon, float speedMph, float heading) {
    hasGPS = true;
    gpsLat = lat;
    gpsLon = lon;
    gpsSpeed = speedMph;
    gpsHeading = heading;
}

void AlertDB::setTimestampUTC(uint32_t unixTime) {
    hasRTC = true;
    rtcTimestamp = unixTime;
}

uint32_t AlertDB::getTotalAlerts() const {
    if (!db) return 0;
    
    sqlite3_stmt* stmt;
    const char* sql = "SELECT COUNT(*) FROM alerts WHERE event = 'ALERT';";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    
    uint32_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    
    return count;
}

String AlertDB::getRecentJson(size_t maxRows) const {
    if (!db) return "[]";
    
    String json = "[";
    
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT timestamp_ms, timestamp_utc, band, frequency, direction, "
        "strength_front, strength_rear, alert_count, muted, "
        "latitude, longitude, speed_mph, event "
        "FROM alerts ORDER BY id DESC LIMIT %zu;",
        maxRows
    );
    
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return "[]";
    }
    
    bool first = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (!first) json += ",";
        first = false;
        
        json += "{";
        json += "\"ts\":" + String(sqlite3_column_int64(stmt, 0));
        
        if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
            json += ",\"utc\":" + String(sqlite3_column_int64(stmt, 1));
        }
        
        if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
            json += ",\"band\":\"" + String((const char*)sqlite3_column_text(stmt, 2)) + "\"";
        }
        
        if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
            json += ",\"freq\":" + String(sqlite3_column_int(stmt, 3));
        }
        
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
            json += ",\"dir\":\"" + String((const char*)sqlite3_column_text(stmt, 4)) + "\"";
        }
        
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
            json += ",\"front\":" + String(sqlite3_column_int(stmt, 5));
        }
        
        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            json += ",\"rear\":" + String(sqlite3_column_int(stmt, 6));
        }
        
        json += ",\"count\":" + String(sqlite3_column_int(stmt, 7));
        json += ",\"muted\":" + String(sqlite3_column_int(stmt, 8) ? "true" : "false");
        
        if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
            json += ",\"lat\":" + String(sqlite3_column_double(stmt, 9), 6);
            json += ",\"lon\":" + String(sqlite3_column_double(stmt, 10), 6);
            json += ",\"speed\":" + String(sqlite3_column_double(stmt, 11), 1);
        }
        
        json += ",\"event\":\"" + String((const char*)sqlite3_column_text(stmt, 12)) + "\"";
        json += "}";
    }
    
    sqlite3_finalize(stmt);
    json += "]";
    
    return json;
}

String AlertDB::getStatsJson() const {
    if (!db) return "{}";
    
    String json = "{";
    
    // Total alerts
    json += "\"total\":" + String(getTotalAlerts());
    
    // Alerts by band
    json += ",\"byBand\":{";
    const char* bands[] = {"Ka", "K", "X", "LASER"};
    for (int i = 0; i < 4; i++) {
        if (i > 0) json += ",";
        
        char sql[128];
        snprintf(sql, sizeof(sql),
            "SELECT COUNT(*) FROM alerts WHERE band = '%s' AND event = 'ALERT';",
            bands[i]
        );
        
        sqlite3_stmt* stmt;
        int count = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
        
        json += "\"" + String(bands[i]) + "\":" + String(count);
    }
    json += "}";
    
    // Session count
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sessions;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            json += ",\"sessions\":" + String(sqlite3_column_int(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    
    // Current session ID
    json += ",\"currentSession\":" + String(sessionId);
    
    json += "}";
    return json;
}

bool AlertDB::clearAll() {
    if (!db) return false;
    
    Serial.println("[AlertDB] WARNING: Clearing all data!");
    
    if (!execSQL("DELETE FROM alerts;")) return false;
    if (!execSQL("DELETE FROM sessions;")) return false;
    if (!execSQL("VACUUM;")) return false;  // Reclaim space
    
    // Start fresh session
    return initSession();
}
