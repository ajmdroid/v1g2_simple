// OBD-II remembered-device persistence — NVS + SD backup, load/save/upsert,
// forget, auto-connect target lookup, and JSON parsing.
// Extracted from obd_handler.cpp for maintainability.

#include "obd_internals.h"
#include "storage_manager.h"

#include <ArduinoJson.h>
#include <algorithm>

// ---------------------------------------------------------------------------
// JSON parsing (shared by load + SD restore)
// ---------------------------------------------------------------------------

size_t parseDevicesJson(const String& blob,
                        std::vector<OBDRememberedDevice>& out,
                        size_t maxDevices) {
    if (blob.length() == 0) return 0;

    JsonDocument doc;
    if (deserializeJson(doc, blob) != DeserializationError::Ok) return 0;

    JsonArray arr = doc["devices"].as<JsonArray>();
    if (arr.isNull()) return 0;

    size_t count = 0;
    for (JsonObject item : arr) {
        if (out.size() >= maxDevices) break;

        const char* addr = item["address"] | "";
        if (!addr || addr[0] == '\0') continue;

        OBDRememberedDevice d;
        d.address = String(addr);
        if (isNullAddress(d.address)) continue;
        d.name = String((const char*)(item["name"] | ""));
        d.pin = String((const char*)(item["pin"] | ""));
        d.autoConnect = item["autoConnect"] | false;
        d.lastSeenMs = item["lastSeenMs"] | 0;
        out.push_back(d);
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Public remembered-device management
// ---------------------------------------------------------------------------

bool OBDHandler::forgetRemembered(const String& address) {
    bool removed = false;
    bool wasTarget = false;

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return false;
        }

        auto it = std::remove_if(rememberedDevices.begin(), rememberedDevices.end(),
                                 [&](const OBDRememberedDevice& d) {
                                     return d.address.equalsIgnoreCase(address);
                                 });
        if (it != rememberedDevices.end()) {
            rememberedDevices.erase(it, rememberedDevices.end());
            removed = true;
        }

        if (hasTargetDevice && String(targetAddress.toString().c_str()).equalsIgnoreCase(address)) {
            wasTarget = true;
            hasTargetDevice = false;
            targetDeviceName = "";
            targetPin = "";
            rememberTargetOnConnect = false;
            targetAutoConnect = false;
        }
    }

    if (removed) {
        saveRememberedDevices();
    }
    if (wasTarget) {
        disconnect();
    }
    return removed;
}

bool OBDHandler::setRememberedAutoConnect(const String& address, bool enabled) {
    bool changed = false;

    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return false;
        }

        for (auto& d : rememberedDevices) {
            if (d.address.equalsIgnoreCase(address)) {
                if (d.autoConnect != enabled) {
                    d.autoConnect = enabled;
                    changed = true;
                }
                break;
            }
        }
    }

    if (changed) {
        saveRememberedDevices();
    }
    return changed;
}

// ---------------------------------------------------------------------------
// NVS + SD load / save
// ---------------------------------------------------------------------------

void OBDHandler::loadRememberedDevices() {
    std::vector<OBDRememberedDevice> loaded;
    loaded.reserve(MAX_REMEMBERED_DEVICES);

    // --- Phase 1: Try NVS ---
    bool nvsOk = false;
    {
        Preferences p;
        if (p.begin("obd_store", true)) {
            String blob;
            if (p.isKey("devices")) {
                blob = p.getString("devices", "");
            }
            p.end();

            if (parseDevicesJson(blob, loaded, MAX_REMEMBERED_DEVICES) > 0) {
                nvsOk = true;
            }
        }
    }

    // --- Phase 2: If NVS empty/corrupt, try SD backup ---
    if (!nvsOk) {
        if (storageManager.isReady() && storageManager.isSDCard()) {
            StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
            if (sdLock) {
                fs::FS* sdFs = storageManager.getFilesystem();
                if (sdFs && sdFs->exists(OBD_SD_BACKUP_PATH)) {
                    File f = sdFs->open(OBD_SD_BACKUP_PATH, "r");
                    if (f) {
                        // Sanity-check file size (< 4KB for 8 devices)
                        if (f.size() > 0 && f.size() < 4096) {
                            String blob = f.readString();
                            f.close();
                            if (parseDevicesJson(blob, loaded, MAX_REMEMBERED_DEVICES) > 0) {
                                Serial.printf("[OBD] Restored %u device(s) from SD backup\n",
                                              (unsigned)loaded.size());
                                // Re-persist to NVS so next boot is fast
                                JsonDocument doc;
                                JsonArray arr = doc["devices"].to<JsonArray>();
                                for (const auto& d : loaded) {
                                    JsonObject o = arr.add<JsonObject>();
                                    o["address"] = d.address;
                                    o["name"] = d.name;
                                    o["pin"] = d.pin;
                                    o["autoConnect"] = d.autoConnect;
                                    o["lastSeenMs"] = d.lastSeenMs;
                                }
                                String nvsBlob;
                                serializeJson(doc, nvsBlob);
                                Preferences p;
                                if (p.begin("obd_store", false)) {
                                    p.putString("devices", nvsBlob);
                                    p.end();
                                }
                            } else {
                                f.close();
                            }
                        } else {
                            f.close();
                        }
                    }
                }
            }
        }
    }

    if (loaded.empty()) {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (lock.ok()) {
            rememberedDevices.clear();
        }
        return;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (lock.ok()) {
        rememberedDevices = std::move(loaded);
    }
}

void OBDHandler::saveRememberedDevices() {
    std::vector<OBDRememberedDevice> snapshot;
    {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (!lock.ok()) {
            return;
        }
        snapshot = rememberedDevices;
    }

    JsonDocument doc;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (const auto& d : snapshot) {
        JsonObject o = arr.add<JsonObject>();
        o["address"] = d.address;
        o["name"] = d.name;
        o["pin"] = d.pin;
        o["autoConnect"] = d.autoConnect;
        o["lastSeenMs"] = d.lastSeenMs;
    }

    String blob;
    serializeJson(doc, blob);

    // --- Save to NVS (primary) ---
    bool nvsOk = false;
    {
        Preferences p;
        if (p.begin("obd_store", false)) {
            p.putString("devices", blob);
            p.end();
            nvsOk = true;
        }
    }

    // --- Backup to SD (secondary) ---
    if (storageManager.isReady() && storageManager.isSDCard()) {
        StorageManager::SDLockBlocking sdLock(storageManager.getSDMutex());
        if (sdLock) {
            fs::FS* sdFs = storageManager.getFilesystem();
            if (sdFs) {
                if (StorageManager::writeJsonFileAtomic(*sdFs, OBD_SD_BACKUP_PATH, doc)) {
                    Serial.printf("[OBD] Backed up %u device(s) to SD\n", (unsigned)snapshot.size());
                } else {
                    Serial.println("[OBD] WARN: SD backup write failed");
                }
            }
        }
    }

    if (!nvsOk) {
        Serial.println("[OBD] WARN: NVS save failed");
    }
}

void OBDHandler::upsertRemembered(const String& address,
                                  const String& name,
                                  const String& pin,
                                  bool autoConnect,
                                  uint32_t lastSeenMs) {
    if (isNullAddressString(address)) {
        return;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return;
    }

    for (auto& d : rememberedDevices) {
        if (d.address.equalsIgnoreCase(address)) {
            if (name.length()) d.name = name;
            d.pin = pin;
            d.autoConnect = autoConnect;
            d.lastSeenMs = lastSeenMs;
            return;
        }
    }

    OBDRememberedDevice d;
    d.address = address;
    d.name = name.length() ? name : address;
    d.pin = pin;
    d.autoConnect = autoConnect;
    d.lastSeenMs = lastSeenMs;

    rememberedDevices.insert(rememberedDevices.begin(), d);
    if (rememberedDevices.size() > MAX_REMEMBERED_DEVICES) {
        rememberedDevices.resize(MAX_REMEMBERED_DEVICES);
    }
}

bool OBDHandler::findAutoConnectTarget(OBDRememberedDevice& out) const {
    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return false;
    }

    for (const auto& d : rememberedDevices) {
        if (d.autoConnect && d.address.length() > 0 && !isNullAddressString(d.address)) {
            out = d;
            return true;
        }
    }
    return false;
}
