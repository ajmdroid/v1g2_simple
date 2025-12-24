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
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    
    if (err) {
        Serial.printf("[V1Profiles] JSON parse error: %s\n", err.c_str());
        return false;
    }
    
    profile.name = name;
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
        
        if (doc.containsKey("xBand")) s.setXBandEnabled(doc["xBand"]);
        if (doc.containsKey("kBand")) s.setKBandEnabled(doc["kBand"]);
        if (doc.containsKey("kaBand")) s.setKaBandEnabled(doc["kaBand"]);
        if (doc.containsKey("laser")) s.setLaserEnabled(doc["laser"]);
        if (doc.containsKey("kuBand")) s.setKuBandEnabled(doc["kuBand"]);
        if (doc.containsKey("euro")) s.setEuroMode(doc["euro"]);
        if (doc.containsKey("kVerifier")) s.setKVerifier(doc["kVerifier"]);
        if (doc.containsKey("laserRear")) s.setLaserRear(doc["laserRear"]);
        if (doc.containsKey("customFreqs")) s.setCustomFreqs(doc["customFreqs"]);
        if (doc.containsKey("kaAlwaysPriority")) s.setKaAlwaysPriority(doc["kaAlwaysPriority"]);
        if (doc.containsKey("fastLaserDetect")) s.setFastLaserDetect(doc["fastLaserDetect"]);
        if (doc.containsKey("kaSensitivity")) s.setKaSensitivity(doc["kaSensitivity"]);
        if (doc.containsKey("kSensitivity")) s.setKSensitivity(doc["kSensitivity"]);
        if (doc.containsKey("xSensitivity")) s.setXSensitivity(doc["xSensitivity"]);
        if (doc.containsKey("autoMute")) s.setAutoMute(doc["autoMute"]);
        if (doc.containsKey("muteToMuteVolume")) s.setMuteToMuteVolume(doc["muteToMuteVolume"]);
        if (doc.containsKey("bogeyLockLoud")) s.setBogeyLockLoud(doc["bogeyLockLoud"]);
        if (doc.containsKey("muteXKRear")) s.setMuteXKRear(doc["muteXKRear"]);
        if (doc.containsKey("startupSequence")) s.setStartupSequence(doc["startupSequence"]);
        if (doc.containsKey("restingDisplay")) s.setRestingDisplay(doc["restingDisplay"]);
        if (doc.containsKey("bsmPlus")) s.setBsmPlus(doc["bsmPlus"]);
        if (doc.containsKey("mrct")) s.setMrct(doc["mrct"]);
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
    
    // Store raw bytes for exact restoration
    JsonArray bytes = doc["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; i++) {
        bytes.add(s.bytes[i]);
    }
    
    // Store display setting (not part of user bytes)
    doc["displayOn"] = profile.displayOn;
    
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
    
    if (settingsObj.containsKey("xBand")) settings.setXBandEnabled(settingsObj["xBand"]);
    if (settingsObj.containsKey("kBand")) settings.setKBandEnabled(settingsObj["kBand"]);
    if (settingsObj.containsKey("kaBand")) settings.setKaBandEnabled(settingsObj["kaBand"]);
    if (settingsObj.containsKey("laser")) settings.setLaserEnabled(settingsObj["laser"]);
    if (settingsObj.containsKey("kuBand")) settings.setKuBandEnabled(settingsObj["kuBand"]);
    if (settingsObj.containsKey("euro")) settings.setEuroMode(settingsObj["euro"]);
    if (settingsObj.containsKey("kVerifier")) settings.setKVerifier(settingsObj["kVerifier"]);
    if (settingsObj.containsKey("laserRear")) settings.setLaserRear(settingsObj["laserRear"]);
    if (settingsObj.containsKey("customFreqs")) settings.setCustomFreqs(settingsObj["customFreqs"]);
    if (settingsObj.containsKey("kaAlwaysPriority")) settings.setKaAlwaysPriority(settingsObj["kaAlwaysPriority"]);
    if (settingsObj.containsKey("fastLaserDetect")) settings.setFastLaserDetect(settingsObj["fastLaserDetect"]);
    if (settingsObj.containsKey("kaSensitivity")) settings.setKaSensitivity(settingsObj["kaSensitivity"]);
    if (settingsObj.containsKey("kSensitivity")) settings.setKSensitivity(settingsObj["kSensitivity"]);
    if (settingsObj.containsKey("xSensitivity")) settings.setXSensitivity(settingsObj["xSensitivity"]);
    if (settingsObj.containsKey("autoMute")) settings.setAutoMute(settingsObj["autoMute"]);
    if (settingsObj.containsKey("muteToMuteVolume")) settings.setMuteToMuteVolume(settingsObj["muteToMuteVolume"]);
    if (settingsObj.containsKey("bogeyLockLoud")) settings.setBogeyLockLoud(settingsObj["bogeyLockLoud"]);
    if (settingsObj.containsKey("muteXKRear")) settings.setMuteXKRear(settingsObj["muteXKRear"]);
    if (settingsObj.containsKey("startupSequence")) settings.setStartupSequence(settingsObj["startupSequence"]);
    if (settingsObj.containsKey("restingDisplay")) settings.setRestingDisplay(settingsObj["restingDisplay"]);
    if (settingsObj.containsKey("bsmPlus")) settings.setBsmPlus(settingsObj["bsmPlus"]);
    if (settingsObj.containsKey("mrct")) settings.setMrct(settingsObj["mrct"]);
    if (settingsObj.containsKey("driveSafe3D")) settings.setDriveSafe3D(settingsObj["driveSafe3D"]);
    if (settingsObj.containsKey("driveSafe3DHD")) settings.setDriveSafe3DHD(settingsObj["driveSafe3DHD"]);
    if (settingsObj.containsKey("redflexHalo")) settings.setRedflexHalo(settingsObj["redflexHalo"]);
    if (settingsObj.containsKey("redflexNK7")) settings.setRedflexNK7(settingsObj["redflexNK7"]);
    if (settingsObj.containsKey("ekin")) settings.setEkin(settingsObj["ekin"]);
    if (settingsObj.containsKey("photoVerifier")) settings.setPhotoVerifier(settingsObj["photoVerifier"]);
    
    Serial.printf("[V1Profiles] After parse - byte0=%02X byte2=%02X\n", settings.bytes[0], settings.bytes[2]);
    Serial.printf("[V1Profiles]   xBand=%d, restingDisplay=%d, bsmPlus=%d\n", 
        settings.xBandEnabled(), settings.restingDisplay(), settings.bsmPlus());
    
    return true;
}
