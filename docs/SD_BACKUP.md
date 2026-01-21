# SD Card Backup System

## Overview

All lockout data (manual zones and auto-learning clusters) is automatically backed up to SD card. This protects your data during:
- Firmware updates/reflashing
- Factory resets
- LittleFS corruption
- Device replacements

## What Gets Backed Up

### 1. Manual Lockout Zones (`/v1simple_lockouts.json`)
```json
{
  "_type": "v1simple_lockouts_backup",
  "_version": 1,
  "timestamp": 1234567890,
  "lockouts": [
    {
      "name": "Safeway Parking",
      "latitude": 37.7749,
      "longitude": -122.4194,
      "radius_m": 150.0,
      "enabled": true,
      "muteX": false,
      "muteK": true,
      "muteKa": false,
      "muteLaser": false
    }
  ]
}
```

### 2. Auto-Learning Clusters (`/v1simple_auto_lockouts.json`)
```json
{
  "_type": "v1simple_auto_lockouts_backup",
  "_version": 1,
  "timestamp": 1234567890,
  "clusters": [
    {
      "name": "K-37.7749,-122.4194",
      "centerLat": 37.7749,
      "centerLon": -122.4194,
      "radius_m": 120.5,
      "band": 1,
      "frequency_khz": 24150,
      "frequency_tolerance_khz": 25,
      "hitCount": 5,
      "stoppedHitCount": 4,
      "movingHitCount": 1,
      "firstSeen": 1705881600,
      "lastSeen": 1706054400,
      "passWithoutAlertCount": 0,
      "isPromoted": true,
      "promotedLockoutIndex": 2,
      "events": [
        // Last 5 events (most recent)
        {
          "lat": 37.7748,
          "lon": -122.4195,
          "band": 1,
          "freq": 24150,
          "signal": 7,
          "duration": 3500,
          "time": 1705881600,
          "moving": false,
          "persistent": true
        }
      ]
    }
  ]
}
```

## Automatic Backup Triggers

Backups happen automatically when:

1. **Adding/editing/deleting manual lockout zones**
   - `lockoutManager.addLockout()` → saves to LittleFS → backs up to SD
   - `lockoutManager.removeLockout()` → saves to LittleFS → backs up to SD
   - `lockoutManager.updateLockout()` → saves to LittleFS → backs up to SD

2. **Promoting auto-lockout clusters**
   - Cluster reaches 2 stopped hits or 4 moving hits
   - Saves to LittleFS → backs up to SD

3. **Demoting auto-lockout clusters**
   - Cluster passes without alert 2 times
   - Cluster stale for 30 days
   - Saves to LittleFS → backs up to SD

4. **Manual save requests**
   - REST API calls `/api/lockouts/save`
   - Web UI "Save" button
   - Serial command `save_lockouts`

## Automatic Restore

On boot, the system checks:

1. **LittleFS has lockout data?**
   - ✅ Yes → Load from LittleFS, keep SD backup as safety net
   - ❌ No → Continue to step 2

2. **SD card has backup?**
   - ✅ Yes → Restore from SD backup, copy to LittleFS
   - ❌ No → Start with empty lockout data

### Boot Sequence
```cpp
void setup() {
  // Initialize storage
  storageManager.begin();  // Mounts SD card (if present) or LittleFS
  
  // Initialize lockout managers
  lockouts.loadFromJSON("/v1profiles/lockouts.json");
  // If file not found, automatically checks SD backup and restores
  
  autoLockouts.loadFromJSON("/v1profiles/auto_lockouts.json");
  // If file not found, automatically checks SD backup and restores
}
```

### Serial Monitor Output
```
[Storage] SD_MMC mounted successfully (16GB)
[Lockout] No lockout file found at /v1profiles/lockouts.json
[Lockout] LittleFS empty, checking for SD backup...
[Lockout] Restored 12 lockouts from SD backup
[Lockout] Saved 12 lockout zones (1856 bytes)
[AutoLockout] No learning data at /v1profiles/auto_lockouts.json
[AutoLockout] LittleFS empty, checking for SD backup...
[AutoLockout] Restored 8 clusters from SD backup
[AutoLockout] Saved 8 clusters (4582 bytes)
```

## Storage Efficiency

### File Sizes
| Item | LittleFS | SD Backup | Notes |
|------|----------|-----------|-------|
| Manual lockouts (10 zones) | ~1.5 KB | ~1.8 KB | Full data |
| Auto-lockout clusters (50) | ~50 KB | ~35 KB | SD: Last 5 events only |
| Total typical | ~52 KB | ~37 KB | Minimal space usage |

### Memory Optimization
- **SD backup saves last 5 events per cluster** (vs 20 in LittleFS)
- Reason: Reduces SD write size, 5 events enough to preserve promotion state
- On restore: Cluster promotion status preserved, older events pruned

## Manual Backup/Restore

### Export to Computer
1. Remove SD card from device
2. Copy files from SD root:
   - `/v1simple_lockouts.json`
   - `/v1simple_auto_lockouts.json`
3. Save to computer for safekeeping

### Import to New Device
1. Copy JSON files to SD card root
2. Insert SD card into new device
3. Flash firmware (LittleFS will be empty)
4. Boot device → automatic restore from SD

## Backup Safety

### Protected Data
- ✅ Lockout zones (name, location, radius, band settings)
- ✅ Learning clusters (hit counts, promotion state, timestamps)
- ✅ Alert history (last 5 events per cluster)

### NOT Backed Up to SD
- ❌ WiFi AP password (security - SD cards can be read elsewhere)
- ❌ BLE pairing keys (device-specific)
- ❌ Display settings (stored in NVS, separate backup system)

## Troubleshooting

### Backup Not Working
**Symptom:** Changes not saved after reflash

**Check:**
1. SD card inserted?
   ```
   [Storage] SD_MMC mounted successfully
   ```
2. SD card writable?
   ```
   ls -la /mnt/sd/v1simple_*.json
   ```
3. Auto-backup enabled?
   ```cpp
   // In lockout_manager.cpp, saveToJSON():
   backupToSD();  // Should be called
   ```

### Restore Not Working
**Symptom:** Data lost after reflash despite SD backup

**Check:**
1. Backup files exist?
   - Look for `/v1simple_lockouts.json` on SD root
   - Look for `/v1simple_auto_lockouts.json` on SD root

2. Backup format valid?
   ```json
   {
     "_type": "v1simple_lockouts_backup",  // Must match
     "_version": 1,
     "lockouts": [...]
   }
   ```

3. Check serial monitor for restore messages:
   ```
   [Lockout] LittleFS empty, checking for SD backup...
   [Lockout] Restored X lockouts from SD backup
   ```

### SD Card Issues
**Symptom:** SD backup fails, "SD card not available"

**Solutions:**
1. **Format SD card:** FAT32, MBR partition table
2. **Check wiring:** Waveshare 3.49 uses SDMMC pins:
   - CLK: GPIO41
   - CMD: GPIO39
   - D0: GPIO40
3. **Test SD mounting:**
   ```cpp
   if (storageManager.isSDCard()) {
     Serial.println("SD OK");
   } else {
     Serial.println("SD failed - using LittleFS only");
   }
   ```

## Best Practices

1. **Use quality SD cards**
   - SanDisk, Samsung, Kingston recommended
   - Class 10 or higher
   - 8-32GB sufficient

2. **Periodic manual exports**
   - Copy SD backup to computer quarterly
   - Protects against SD card failure

3. **Test restores**
   - After major changes, test reflash + restore
   - Verify lockout count matches before reflash

4. **Keep SD card inserted**
   - Backups only work if SD is present
   - Device works without SD (LittleFS only)

## Future Enhancements

Potential additions:
- [ ] Cloud backup via WiFi
- [ ] Backup rotation (keep last 3 versions)
- [ ] Compressed backups (gzip)
- [ ] Backup encryption
- [ ] Import/export via web UI
- [ ] Backup status indicator on display
