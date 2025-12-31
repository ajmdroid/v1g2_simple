/**
 * V1 Profile Manager Implementation
 */

#include "v1_profiles.h"
#include <ArduinoJson.h>

// Global instance
V1ProfileManager v1ProfileManager;

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
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    
    if (err) {
        Serial.printf("[V1Profiles] JSON parse error: %s\n", err.c_str());
        return false;
    }
    
    profile.name = name;
    profile.description = doc["description"] | "";
    profile.displayOn = doc["displayOn"] | true;  // Default to on
    
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

bool V1ProfileManager::saveProfile(const V1Profile& profile) {
    if (!ready || !fs) {
        return false;
    }
    
    String path = profilePath(profile.name);
    File file = fs->open(path, FILE_WRITE);
    if (!file) {
        Serial.printf("[V1Profiles] Failed to create profile: %s\n", path.c_str());
        return false;
    }
    
    JsonDocument doc;
    const V1UserSettings& s = profile.settings;
    
    // Store metadata
    doc["name"] = profile.name;
    doc["description"] = profile.description;
    doc["displayOn"] = profile.displayOn;
    
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
    
    serializeJsonPretty(doc, file);
    file.close();
    
    Serial.printf("[V1Profiles] Saved profile: %s\n", profile.name.c_str());
    return true;
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
    if (!saveProfile(profile)) {
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
