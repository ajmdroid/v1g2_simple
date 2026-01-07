/**
 * V1 Profile Manager Implementation
 */

#include "v1_profiles.h"
#include <ArduinoJson.h>

// Global instance
V1ProfileManager v1ProfileManager;

// CRC32 lookup table (standard polynomial 0xEDB88320)
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
    0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
    0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
    0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
    0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
    0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
    0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
    0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
    0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
    0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
    0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
    0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
    0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
    0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
    0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
    0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
    0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
    0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
    0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
    0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
    0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
    0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
    0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
    0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
    0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
    0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
    0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
    0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD706B3,
    0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

uint32_t V1ProfileManager::calculateCRC32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

V1ProfileManager::V1ProfileManager() 
    : fs(nullptr)
    , ready(false)
    , profileDir("/v1profiles")
    , currentValid(false) {
}

bool V1ProfileManager::begin(fs::FS* filesystem) {
    if (!filesystem) {
        Serial.println("[V1Profiles] No filesystem provided");
        return false;
    }
    
    fs = filesystem;
    
    // Create profiles directory if it doesn't exist
    if (!fs->exists(profileDir)) {
        if (!fs->mkdir(profileDir)) {
            Serial.println("[V1Profiles] Failed to create profiles directory");
            return false;
        }
        Serial.println("[V1Profiles] Created profiles directory");
    }
    
    ready = true;
    Serial.println("[V1Profiles] Initialized");
    return true;
}

String V1ProfileManager::profilePath(const String& name) const {
    // Sanitize name for filesystem
    String safeName = name;
    safeName.replace("/", "_");
    safeName.replace("\\", "_");
    safeName.replace("..", "_");
    return profileDir + "/" + safeName + ".json";
}

std::vector<String> V1ProfileManager::listProfiles() const {
    std::vector<String> profiles;
    
    if (!ready || !fs) {
        return profiles;
    }
    
    File dir = fs->open(profileDir);
    if (!dir || !dir.isDirectory()) {
        return profiles;
    }
    
    File entry;
    while ((entry = dir.openNextFile())) {
        String name = entry.name();
        if (name.endsWith(".json")) {
            // Remove .json extension and path
            int lastSlash = name.lastIndexOf('/');
            if (lastSlash >= 0) {
                name = name.substring(lastSlash + 1);
            }
            name = name.substring(0, name.length() - 5);  // Remove .json
            profiles.push_back(name);
        }
        entry.close();
    }
    dir.close();
    
    return profiles;
}

bool V1ProfileManager::loadProfile(const String& name, V1Profile& profile) const {
    if (!ready || !fs) {
        return false;
    }
    
    String path = profilePath(name);
    File file = fs->open(path, FILE_READ);
    if (!file) {
        Serial.printf("[V1Profiles] Failed to open profile: %s\n", path.c_str());
        return false;
    }
    
    // Hard cap JSON size to avoid excessive allocation on small devices
    if (file.size() > 4096) {
        Serial.printf("[V1Profiles] Profile too large (%u bytes), aborting\n", (unsigned)file.size());
        file.close();
        return false;
    }
    
    // Read file content for CRC validation
    size_t fileSize = file.size();
    uint8_t* fileContent = new uint8_t[fileSize];
    file.read(fileContent, fileSize);
    file.close();
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, fileContent, fileSize);
    
    if (err) {
        delete[] fileContent;
        lastError = String("JSON parse error: ") + err.c_str();
        Serial.printf("[V1Profiles] %s\n", lastError.c_str());
        return false;
    }
    
    // Validate CRC32 if present
    if (doc["crc32"].is<uint32_t>()) {
        uint32_t storedCrc = doc["crc32"].as<uint32_t>();
        
        // Calculate CRC of the 6 settings bytes
        JsonArray bytesArr = doc["bytes"];
        if (bytesArr && bytesArr.size() == 6) {
            uint8_t settingsBytes[6];
            for (int i = 0; i < 6; i++) {
                settingsBytes[i] = bytesArr[i].as<uint8_t>();
            }
            uint32_t computedCrc = calculateCRC32(settingsBytes, 6);
            if (storedCrc != computedCrc) {
                delete[] fileContent;
                lastError = "CRC mismatch - profile file corrupted";
                Serial.printf("[V1Profiles] %s (stored: %08X, computed: %08X)\n", 
                    lastError.c_str(), storedCrc, computedCrc);
                return false;
            }
            Serial.println("[V1Profiles] CRC32 validated OK");
        }
    }
    
    delete[] fileContent;
    
    profile.name = name;
    profile.description = doc["description"] | "";
    profile.displayOn = doc["displayOn"] | true;  // Default to on
    profile.mainVolume = doc["mainVolume"] | 0xFF;  // 0xFF = don't change
    profile.mutedVolume = doc["mutedVolume"] | 0xFF;  // 0xFF = don't change
    
    // Parse settings bytes
    JsonArray bytes = doc["bytes"];
    if (bytes && bytes.size() == 6) {
        for (int i = 0; i < 6; i++) {
            profile.settings.bytes[i] = bytes[i].as<uint8_t>();
        }
    } else {
        // Try individual settings (legacy or human-readable format)
        V1UserSettings& s = profile.settings;
        s.setDefaults();
        
        if (!doc["xBand"].isNull()) s.setXBandEnabled(doc["xBand"]);
        if (!doc["kBand"].isNull()) s.setKBandEnabled(doc["kBand"]);
        if (!doc["kaBand"].isNull()) s.setKaBandEnabled(doc["kaBand"]);
        if (!doc["laser"].isNull()) s.setLaserEnabled(doc["laser"]);
        if (!doc["kuBand"].isNull()) s.setKuBandEnabled(doc["kuBand"]);
        if (!doc["euro"].isNull()) s.setEuroMode(doc["euro"]);
        if (!doc["kVerifier"].isNull()) s.setKVerifier(doc["kVerifier"]);
        if (!doc["laserRear"].isNull()) s.setLaserRear(doc["laserRear"]);
        if (!doc["customFreqs"].isNull()) s.setCustomFreqs(doc["customFreqs"]);
        if (!doc["kaAlwaysPriority"].isNull()) s.setKaAlwaysPriority(doc["kaAlwaysPriority"]);
        if (!doc["fastLaserDetect"].isNull()) s.setFastLaserDetect(doc["fastLaserDetect"]);
        if (!doc["kaSensitivity"].isNull()) s.setKaSensitivity(doc["kaSensitivity"]);
        if (!doc["kSensitivity"].isNull()) s.setKSensitivity(doc["kSensitivity"]);
        if (!doc["xSensitivity"].isNull()) s.setXSensitivity(doc["xSensitivity"]);
        if (!doc["autoMute"].isNull()) s.setAutoMute(doc["autoMute"]);
        if (!doc["muteToMuteVolume"].isNull()) s.setMuteToMuteVolume(doc["muteToMuteVolume"]);
        if (!doc["bogeyLockLoud"].isNull()) s.setBogeyLockLoud(doc["bogeyLockLoud"]);
        if (!doc["muteXKRear"].isNull()) s.setMuteXKRear(doc["muteXKRear"]);
        if (!doc["startupSequence"].isNull()) s.setStartupSequence(doc["startupSequence"]);
        if (!doc["restingDisplay"].isNull()) s.setRestingDisplay(doc["restingDisplay"]);
        if (!doc["bsmPlus"].isNull()) s.setBsmPlus(doc["bsmPlus"]);
        if (!doc["mrct"].isNull()) s.setMrct(doc["mrct"]);
    }
    
    Serial.printf("[V1Profiles] Loaded profile: %s\n", name.c_str());
    return true;
}

ProfileSaveResult V1ProfileManager::saveProfile(const V1Profile& profile) {
    if (!ready || !fs) {
        lastError = "Filesystem not ready";
        Serial.printf("[V1Profiles] Save failed: %s\n", lastError.c_str());
        return ProfileSaveResult(false, lastError);
    }
    
    String path = profilePath(profile.name);
    String tmpPath = path + ".tmp";
    String bakPath = path + ".bak";
    
    // Step 1: Write to temporary file (don't truncate original yet)
    File file = fs->open(tmpPath, FILE_WRITE);
    if (!file) {
        lastError = "Failed to create temp file: " + tmpPath;
        Serial.printf("[V1Profiles] %s\n", lastError.c_str());
        return ProfileSaveResult(false, lastError);
    }
    
    JsonDocument doc;
    const V1UserSettings& s = profile.settings;
    
    // Store metadata
    doc["name"] = profile.name;
    doc["description"] = profile.description;
    doc["displayOn"] = profile.displayOn;
    doc["mainVolume"] = profile.mainVolume;
    doc["mutedVolume"] = profile.mutedVolume;
    
    // Store raw bytes for exact restoration
    JsonArray bytes = doc["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }

    
    // Also store human-readable settings
    doc["xBand"] = s.xBandEnabled();
    doc["kBand"] = s.kBandEnabled();
    doc["kaBand"] = s.kaBandEnabled();
    doc["laser"] = s.laserEnabled();
    doc["kuBand"] = s.kuBandEnabled();
    doc["euro"] = s.euroMode();
    doc["kVerifier"] = s.kVerifier();
    doc["laserRear"] = s.laserRear();
    doc["customFreqs"] = s.customFreqs();
    doc["kaAlwaysPriority"] = s.kaAlwaysPriority();
    doc["fastLaserDetect"] = s.fastLaserDetect();
    doc["kaSensitivity"] = s.kaSensitivity();
    doc["kSensitivity"] = s.kSensitivity();
    doc["xSensitivity"] = s.xSensitivity();
    doc["autoMute"] = s.autoMute();
    doc["muteToMuteVolume"] = s.muteToMuteVolume();
    doc["bogeyLockLoud"] = s.bogeyLockLoud();
    doc["muteXKRear"] = s.muteXKRear();
    doc["startupSequence"] = s.startupSequence();
    doc["restingDisplay"] = s.restingDisplay();
    doc["bsmPlus"] = s.bsmPlus();
    doc["mrct"] = s.mrct();
    doc["driveSafe3D"] = s.driveSafe3D();
    doc["driveSafe3DHD"] = s.driveSafe3DHD();
    doc["redflexHalo"] = s.redflexHalo();
    doc["redflexNK7"] = s.redflexNK7();
    doc["ekin"] = s.ekin();
    doc["photoVerifier"] = s.photoVerifier();
    
    // Calculate and store CRC32 of the settings bytes for integrity checking
    uint32_t crc = calculateCRC32(s.bytes, 6);
    doc["crc32"] = crc;
    
    size_t written = serializeJsonPretty(doc, file);
    
    // Step 2: Flush to ensure data is written to SD before closing
    file.flush();
    file.close();
    
    // Step 3: Verify write succeeded
    if (written == 0) {
        lastError = "Serialization failed - no data written";
        Serial.printf("[V1Profiles] %s\n", lastError.c_str());
        fs->remove(tmpPath);
        return ProfileSaveResult(false, lastError);
    }
    
    // Step 4: Create backup of existing file before replacement
    if (fs->exists(path)) {
        // Remove old backup if exists
        if (fs->exists(bakPath)) {
            fs->remove(bakPath);
        }
        // Rename current to backup (for rollback capability)
        if (!fs->rename(path, bakPath)) {
            Serial.printf("[V1Profiles] Warning: Could not create backup: %s\n", bakPath.c_str());
            // Continue anyway - this is not fatal
        } else {
            Serial.printf("[V1Profiles] Created backup: %s\n", bakPath.c_str());
        }
    }
    
    // Step 5: Rename temp to final
    if (!fs->rename(tmpPath, path)) {
        lastError = "Failed to rename temp to final: " + tmpPath + " -> " + path;
        Serial.printf("[V1Profiles] %s\n", lastError.c_str());
        
        // Try to restore from backup
        if (fs->exists(bakPath)) {
            if (fs->rename(bakPath, path)) {
                Serial.println("[V1Profiles] Restored from backup after failed save");
            }
        }
        fs->remove(tmpPath);
        return ProfileSaveResult(false, lastError);
    }
    
    // Step 6: Remove backup after successful save (optional - keep for extra safety)
    // fs->remove(bakPath);  // Uncomment to remove backup after success
    
    Serial.printf("[V1Profiles] Saved profile: %s (%u bytes, CRC: %08X)\n", 
        profile.name.c_str(), written, crc);
    return ProfileSaveResult(true);
}

bool V1ProfileManager::deleteProfile(const String& name) {
    if (!ready || !fs) {
        return false;
    }
    
    String path = profilePath(name);
    if (!fs->exists(path)) {
        return false;
    }
    
    bool ok = fs->remove(path);
    if (ok) {
        Serial.printf("[V1Profiles] Deleted profile: %s\n", name.c_str());
    }
    return ok;
}

bool V1ProfileManager::renameProfile(const String& oldName, const String& newName) {
    if (!ready || !fs) {
        return false;
    }
    
    V1Profile profile;
    if (!loadProfile(oldName, profile)) {
        return false;
    }
    
    profile.name = newName;
    ProfileSaveResult result = saveProfile(profile);
    if (!result.success) {
        return false;
    }
    
    return deleteProfile(oldName);
}

void V1ProfileManager::setCurrentSettings(const uint8_t* bytes) {
    memcpy(currentSettings.bytes, bytes, 6);
    currentValid = true;
    Serial.printf("[V1Profiles] Updated current settings: %02X %02X %02X %02X %02X %02X\n",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
}

String V1ProfileManager::settingsToJson(const V1UserSettings& s) const {
    JsonDocument doc;
    
    // Raw bytes
    JsonArray bytes = doc["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }
    
    // Human-readable
    doc["xBand"] = s.xBandEnabled();
    doc["kBand"] = s.kBandEnabled();
    doc["kaBand"] = s.kaBandEnabled();
    doc["laser"] = s.laserEnabled();
    doc["kuBand"] = s.kuBandEnabled();
    doc["euro"] = s.euroMode();
    doc["kVerifier"] = s.kVerifier();
    doc["laserRear"] = s.laserRear();
    doc["customFreqs"] = s.customFreqs();
    doc["kaAlwaysPriority"] = s.kaAlwaysPriority();
    doc["fastLaserDetect"] = s.fastLaserDetect();
    doc["kaSensitivity"] = s.kaSensitivity();
    doc["kSensitivity"] = s.kSensitivity();
    doc["xSensitivity"] = s.xSensitivity();
    doc["autoMute"] = s.autoMute();
    doc["muteToMuteVolume"] = s.muteToMuteVolume();
    doc["bogeyLockLoud"] = s.bogeyLockLoud();
    doc["muteXKRear"] = s.muteXKRear();
    doc["startupSequence"] = s.startupSequence();
    doc["restingDisplay"] = s.restingDisplay();
    doc["bsmPlus"] = s.bsmPlus();
    doc["mrct"] = s.mrct();
    doc["driveSafe3D"] = s.driveSafe3D();
    doc["driveSafe3DHD"] = s.driveSafe3DHD();
    doc["redflexHalo"] = s.redflexHalo();
    doc["redflexNK7"] = s.redflexNK7();
    doc["ekin"] = s.ekin();
    doc["photoVerifier"] = s.photoVerifier();
    
    String output;
    serializeJson(doc, output);
    return output;
}

String V1ProfileManager::profileToJson(const V1Profile& profile) const {
    JsonDocument doc;
    doc["name"] = profile.name;
    doc["description"] = profile.description;
    doc["displayOn"] = profile.displayOn;
    doc["mainVolume"] = profile.mainVolume;
    doc["mutedVolume"] = profile.mutedVolume;
    
    JsonObject settings = doc["settings"].to<JsonObject>();
    const V1UserSettings& s = profile.settings;
    
    JsonArray bytes = settings["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }
    
    settings["xBand"] = s.xBandEnabled();
    settings["kBand"] = s.kBandEnabled();
    settings["kaBand"] = s.kaBandEnabled();
    settings["laser"] = s.laserEnabled();
    settings["kuBand"] = s.kuBandEnabled();
    settings["euro"] = s.euroMode();
    settings["kVerifier"] = s.kVerifier();
    settings["laserRear"] = s.laserRear();
    settings["customFreqs"] = s.customFreqs();
    settings["kaAlwaysPriority"] = s.kaAlwaysPriority();
    settings["fastLaserDetect"] = s.fastLaserDetect();
    settings["kaSensitivity"] = s.kaSensitivity();
    settings["kSensitivity"] = s.kSensitivity();
    settings["xSensitivity"] = s.xSensitivity();
    settings["autoMute"] = s.autoMute();
    settings["muteToMuteVolume"] = s.muteToMuteVolume();
    settings["bogeyLockLoud"] = s.bogeyLockLoud();
    settings["muteXKRear"] = s.muteXKRear();
    settings["startupSequence"] = s.startupSequence();
    settings["restingDisplay"] = s.restingDisplay();
    settings["bsmPlus"] = s.bsmPlus();
    settings["mrct"] = s.mrct();
    settings["driveSafe3D"] = s.driveSafe3D();
    settings["driveSafe3DHD"] = s.driveSafe3DHD();
    settings["redflexHalo"] = s.redflexHalo();
    settings["redflexNK7"] = s.redflexNK7();
    settings["ekin"] = s.ekin();
    settings["photoVerifier"] = s.photoVerifier();
    
    String output;
    serializeJson(doc, output);
    return output;
}

bool V1ProfileManager::jsonToSettings(const String& json, V1UserSettings& settings) const {
    if (json.length() > 4096) {
        Serial.println("[V1Profiles] JSON too large, rejecting");
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("[V1Profiles] JSON parse error: %s\n", err.c_str());
        return false;
    }
    
    // Check if settings are nested inside a "settings" object
    JsonObject settingsObj = doc["settings"].as<JsonObject>();
    if (settingsObj.isNull()) {
        // Settings are at root level
        settingsObj = doc.as<JsonObject>();
    }
    
    return jsonToSettings(settingsObj, settings);
}

bool V1ProfileManager::jsonToSettings(const JsonObject& settingsObj, V1UserSettings& settings) const {
    // Try raw bytes first (skip if empty to use individual settings)
    JsonArray bytes = settingsObj["bytes"];
    if (bytes && bytes.size() == 6) {
        for (int i = 0; i < 6; i++) {
            settings.bytes[i] = bytes[i].as<uint8_t>();
        }
        Serial.println("[V1Profiles] Loaded from raw bytes");
        return true;
    }

    // Parse individual settings
    settings.setDefaults();
    Serial.println("[V1Profiles] Parsing individual settings");
    bool anyField = false;
    
    if (!settingsObj["xBand"].isNull()) { settings.setXBandEnabled(settingsObj["xBand"]); anyField = true; }
    if (!settingsObj["kBand"].isNull()) { settings.setKBandEnabled(settingsObj["kBand"]); anyField = true; }
    if (!settingsObj["kaBand"].isNull()) { settings.setKaBandEnabled(settingsObj["kaBand"]); anyField = true; }
    if (!settingsObj["laser"].isNull()) { settings.setLaserEnabled(settingsObj["laser"]); anyField = true; }
    if (!settingsObj["kuBand"].isNull()) { settings.setKuBandEnabled(settingsObj["kuBand"]); anyField = true; }
    if (!settingsObj["euro"].isNull()) { settings.setEuroMode(settingsObj["euro"]); anyField = true; }
    if (!settingsObj["kVerifier"].isNull()) { settings.setKVerifier(settingsObj["kVerifier"]); anyField = true; }
    if (!settingsObj["laserRear"].isNull()) { settings.setLaserRear(settingsObj["laserRear"]); anyField = true; }
    if (!settingsObj["customFreqs"].isNull()) { settings.setCustomFreqs(settingsObj["customFreqs"]); anyField = true; }
    if (!settingsObj["kaAlwaysPriority"].isNull()) { settings.setKaAlwaysPriority(settingsObj["kaAlwaysPriority"]); anyField = true; }
    if (!settingsObj["fastLaserDetect"].isNull()) { settings.setFastLaserDetect(settingsObj["fastLaserDetect"]); anyField = true; }
    if (!settingsObj["kaSensitivity"].isNull()) { settings.setKaSensitivity(settingsObj["kaSensitivity"]); anyField = true; }
    if (!settingsObj["kSensitivity"].isNull()) { settings.setKSensitivity(settingsObj["kSensitivity"]); anyField = true; }
    if (!settingsObj["xSensitivity"].isNull()) { settings.setXSensitivity(settingsObj["xSensitivity"]); anyField = true; }
    if (!settingsObj["autoMute"].isNull()) { settings.setAutoMute(settingsObj["autoMute"]); anyField = true; }
    if (!settingsObj["muteToMuteVolume"].isNull()) { settings.setMuteToMuteVolume(settingsObj["muteToMuteVolume"]); anyField = true; }
    if (!settingsObj["bogeyLockLoud"].isNull()) { settings.setBogeyLockLoud(settingsObj["bogeyLockLoud"]); anyField = true; }
    if (!settingsObj["muteXKRear"].isNull()) { settings.setMuteXKRear(settingsObj["muteXKRear"]); anyField = true; }
    if (!settingsObj["startupSequence"].isNull()) { settings.setStartupSequence(settingsObj["startupSequence"]); anyField = true; }
    if (!settingsObj["restingDisplay"].isNull()) { settings.setRestingDisplay(settingsObj["restingDisplay"]); anyField = true; }
    if (!settingsObj["bsmPlus"].isNull()) { settings.setBsmPlus(settingsObj["bsmPlus"]); anyField = true; }
    if (!settingsObj["mrct"].isNull()) { settings.setMrct(settingsObj["mrct"]); anyField = true; }
    if (!settingsObj["driveSafe3D"].isNull()) { settings.setDriveSafe3D(settingsObj["driveSafe3D"]); anyField = true; }
    if (!settingsObj["driveSafe3DHD"].isNull()) { settings.setDriveSafe3DHD(settingsObj["driveSafe3DHD"]); anyField = true; }
    if (!settingsObj["redflexHalo"].isNull()) { settings.setRedflexHalo(settingsObj["redflexHalo"]); anyField = true; }
    if (!settingsObj["redflexNK7"].isNull()) { settings.setRedflexNK7(settingsObj["redflexNK7"]); anyField = true; }
    if (!settingsObj["ekin"].isNull()) { settings.setEkin(settingsObj["ekin"]); anyField = true; }
    if (!settingsObj["photoVerifier"].isNull()) { settings.setPhotoVerifier(settingsObj["photoVerifier"]); anyField = true; }

    if (!anyField) {
        Serial.println("[V1Profiles] No settings provided");
        return false;
    }
    
    Serial.printf("[V1Profiles] After parse - byte0=%02X byte2=%02X\n", settings.bytes[0], settings.bytes[2]);
    Serial.printf("[V1Profiles]   xBand=%d, restingDisplay=%d, bsmPlus=%d\n", 
        settings.xBandEnabled(), settings.restingDisplay(), settings.bsmPlus());
    
    return true;
}
