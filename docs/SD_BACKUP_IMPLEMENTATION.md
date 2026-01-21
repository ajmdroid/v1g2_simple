# SD Backup Implementation Summary

## What Was Added

Automatic SD card backup system for all lockout data to protect against firmware updates, reflashes, and storage corruption.

## Files Modified

### Headers
1. **src/lockout_manager.h**
   - Added `backupToSD()` method
   - Added `restoreFromSD()` method
   - Added `checkAndRestoreFromSD()` method

2. **src/auto_lockout_manager.h**
   - Added `LockoutManager* lockoutManager` member
   - Added `setLockoutManager()` method
   - Added `backupToSD()` method
   - Added `restoreFromSD()` method
   - Added `checkAndRestoreFromSD()` method

### Implementation
3. **src/lockout_manager.cpp**
   - Implemented `backupToSD()` - writes `/v1simple_lockouts.json` to SD root
   - Implemented `restoreFromSD()` - reads SD backup, restores to LittleFS
   - Implemented `checkAndRestoreFromSD()` - auto-restores on boot if LittleFS empty
   - Modified `saveToJSON()` - calls `backupToSD()` after LittleFS save
   - Modified `loadFromJSON()` - calls `checkAndRestoreFromSD()` if file not found

4. **src/auto_lockout_manager.cpp**
   - Added `#include "storage_manager.h"`
   - Implemented `backupToSD()` - writes `/v1simple_auto_lockouts.json` to SD root
     - Saves last 5 events per cluster (vs 20 in LittleFS) for space efficiency
   - Implemented `restoreFromSD()` - reads SD backup, restores to LittleFS
   - Implemented `checkAndRestoreFromSD()` - auto-restores on boot if LittleFS empty
   - Modified `saveToJSON()` - calls `backupToSD()` after LittleFS save
   - Modified `loadFromJSON()` - calls `checkAndRestoreFromSD()` if file not found

## Documentation Added

5. **docs/SD_BACKUP.md** - Complete guide covering:
   - What gets backed up (manual zones, auto-learning clusters)
   - Automatic backup triggers (add/edit/remove lockouts, promotion/demotion)
   - Automatic restore on boot (if LittleFS empty)
   - Storage efficiency (file sizes, memory optimization)
   - Manual backup/restore procedures
   - Troubleshooting guide
   - Best practices

6. **CLAUDE.md** - Updated "File Locations" section:
   - Added SD backup file paths
   - Explained automatic backup/restore behavior
   - Listed what is NOT backed up (WiFi password, BLE keys)
   - Referenced SD_BACKUP.md for details

## How It Works

### Automatic Backup Flow
```
User action (add/edit/remove lockout)
  ↓
saveToJSON() writes to LittleFS
  ↓
backupToSD() writes to SD card
  ↓
Serial log: "[Lockout] Backed up X lockouts to SD (Y bytes)"
```

### Automatic Restore Flow
```
Device boots after reflash
  ↓
loadFromJSON() checks LittleFS
  ↓
File not found? → checkAndRestoreFromSD()
  ↓
SD backup exists? → restoreFromSD()
  ↓
Restore to LittleFS
  ↓
Serial log: "[Lockout] Restored X lockouts from SD backup"
```

## Storage Locations

| Data | Primary (LittleFS) | Backup (SD) |
|------|-------------------|-------------|
| Manual lockouts | `/v1profiles/lockouts.json` | `/v1simple_lockouts.json` |
| Auto-learning clusters | `/v1profiles/auto_lockouts.json` | `/v1simple_auto_lockouts.json` |

## Security Considerations

**Backed Up:**
- ✅ Lockout zone locations, names, radii
- ✅ Band-specific muting settings
- ✅ Learning cluster hit counts, timestamps
- ✅ Alert history (last 5 events per cluster on SD)

**NOT Backed Up to SD:**
- ❌ WiFi AP password (security risk - SD cards removable)
- ❌ BLE pairing keys (device-specific, can't transfer)
- ❌ Display settings (stored in NVS, separate backup)

## Storage Efficiency

### File Sizes
```
Manual lockouts (10 zones):
  LittleFS: ~1.5 KB (full data)
  SD:       ~1.8 KB (full data + metadata)

Auto-learning clusters (50):
  LittleFS: ~50 KB (20 events per cluster)
  SD:       ~35 KB (5 events per cluster)

Total:
  LittleFS: ~52 KB
  SD:       ~37 KB
```

### Why 5 Events on SD vs 20 on LittleFS?
- Reduces SD card writes (better for wear leveling)
- 5 events sufficient to preserve promotion state
- Cluster metadata (hit counts, timestamps) is complete
- Full event history remains in LittleFS for current operation

## Testing Checklist

### Test Backup
1. [ ] Add manual lockout zone via web UI
2. [ ] Check serial: "[Lockout] Backed up X lockouts to SD..."
3. [ ] Remove SD card, verify `/v1simple_lockouts.json` exists
4. [ ] Open file, verify JSON structure correct

### Test Restore
1. [ ] Flash firmware (erases LittleFS)
2. [ ] Boot with SD card containing backup
3. [ ] Check serial: "[Lockout] Restored X lockouts from SD backup"
4. [ ] Verify lockout zones appear in web UI
5. [ ] Verify alert muting works at lockout locations

### Test Auto-Learning Backup
1. [ ] Drive past false alert location 2+ times
2. [ ] Cluster gets promoted to lockout
3. [ ] Check serial: "[AutoLockout] Backed up X clusters to SD..."
4. [ ] Reflash firmware
5. [ ] Check serial: "[AutoLockout] Restored X clusters from SD backup"
6. [ ] Verify promoted cluster still exists

## Future Enhancements

Potential improvements:
- [ ] Backup versioning (keep last 3 backups)
- [ ] Compressed backups (gzip JSON)
- [ ] Cloud backup via WiFi
- [ ] Web UI export/import
- [ ] Backup status indicator on display
- [ ] Scheduled periodic backups

## Notes

- SD card is **optional** - device works fine without it (LittleFS only)
- Backups are **automatic** - no user action required
- Backup files use different names to avoid conflicts with other V1 apps
- Format: `v1simple_*.json` prefix distinguishes from V1 Driver backups
- SD card should be FAT32, MBR partition table
- Waveshare 3.49 uses SDMMC interface (faster than SPI SD cards)
