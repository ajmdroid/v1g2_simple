# V1-Simple Troubleshooting Guide

Quick solutions for common issues with the V1-Simple device.

---

## Table of Contents

1. [Connection Issues](#connection-issues)
2. [Display Problems](#display-problems)
3. [GPS Issues](#gps-issues)
4. [Auto-Lockout Issues](#auto-lockout-issues)
5. [Camera Alerts](#camera-alerts)
6. [OBD/Speed Issues](#obdspeed-issues)
7. [Audio Problems](#audio-problems)
8. [Performance Issues](#performance-issues)
9. [Factory Reset](#factory-reset)

---

## Connection Issues

### Can't find V1-Simple WiFi network

**Symptoms**: `V1-Simple` SSID not visible on phone/laptop

**Solutions**:
1. **Power cycle the device** - Press and hold power button for 5 seconds
2. **Check WiFi at Boot setting** - WiFi may be disabled:
   - Connect via USB serial
   - Or wait 30 seconds, device enables WiFi automatically after timeout
3. **Move closer** - ESP32 WiFi range is limited to ~30 feet
4. **Check for interference** - Other 2.4GHz devices may interfere

### Can't connect to V1-Simple WiFi

**Symptoms**: Password rejected or connection drops

**Solutions**:
1. **Use correct password**: Default is `setupv1g2`
2. **Forget and reconnect**: Remove saved network, reconnect fresh
3. **Check password length**: Must be at least 8 characters
4. **Disable 5GHz**: Some phones try 5GHz first; disable it temporarily

### V1 Won't Connect via BLE

**Symptoms**: "Scanning..." or "No V1 detected" in web UI

**Solutions**:
1. **Ensure V1 Bluetooth is ON**: 
   - On V1: Menu → Expert → Bluetooth → ON
   - V1 must be in "Visible" mode, not "Hidden"
2. **Power cycle V1**: Turn off and back on
3. **Power cycle V1-Simple**: Restart the device
4. **Distance**: Keep V1 within 3 feet during initial pairing
5. **Remove other pairings**: Disconnect V1 from phone apps first

### Frequent BLE Disconnections

**Symptoms**: V1 connects but drops every few minutes

**Solutions**:
1. **Reduce distance**: Keep V1-Simple near the V1
2. **Check battery**: Low V1-Simple battery affects BLE stability
3. **Disable proxy mode**: BLE proxy increases disconnect risk
4. **Check interference**: Other BLE devices nearby can cause issues

---

## Display Problems

### Display is blank/black

**Symptoms**: No display output, LEDs may be on

**Solutions**:
1. **Check brightness**: Touch the display to wake it
2. **Connect to WiFi**: Check settings for display issues
3. **Power cycle**: Hold power for 10 seconds, release

### Display colors wrong/inverted

**Symptoms**: Colors don't match expected appearance

**Solutions**:
1. **Reset colors**: Settings → Display Colors → Reset to Default
2. **Check display style**: Try different display styles (0-4)
3. **Clear preview**: If testing colors, send clear preview command

### Touch not responding

**Symptoms**: Screen shows content but doesn't respond to touch

**Solutions**:
1. **Clean screen**: Fingerprints/debris can affect capacitive touch
2. **Restart device**: Touch controller may need reset
3. **Check for screen protector**: Thick protectors can block touch

---

## GPS Issues

### GPS shows "No Fix"

**Symptoms**: GPS never acquires satellites

**Solutions**:
1. **Wait longer**: First fix can take 2-5 minutes (cold start)
2. **Go outside**: GPS needs clear sky view
3. **Check module**: Not all units have GPS installed
4. **Reset GPS**: Settings → GPS → Reset GPS Module

### GPS position is wrong/drifting

**Symptoms**: Location jumps around or is consistently offset

**Solutions**:
1. **Check HDOP**: High HDOP (>3) means poor accuracy
2. **Improve sky view**: Move away from buildings/trees
3. **Wait for convergence**: Accuracy improves after a few minutes
4. **Check antenna**: Internal antenna may be partially blocked

### Speed doesn't match speedometer

**Symptoms**: GPS speed differs from vehicle speedometer

**Solutions**:
1. **This is normal**: Vehicle speedometers typically read 2-5% high
2. **Use OBD**: OBD speed is more accurate than GPS in some conditions
3. **Check units**: Ensure both are showing same units (mph/kph)

---

## Auto-Lockout Issues

### Lockouts not learning

**Symptoms**: Passes same false alert multiple times without lockout

**Solutions**:
1. **Check settings**: Ensure `lockoutEnabled` is true
2. **Check learn count**: Default requires 2 passes
3. **Check interval**: Must pass within `lockoutLearnIntervalHours` (default 24h)
4. **Check distance**: Must be within `lockoutMaxDistanceM` (default 50m)
5. **Check signal**: Must be below `lockoutMaxSignalStrength` (default 6 bars)
6. **Ka protection**: Ka band has stricter requirements if enabled

### Lockouts learning too aggressively

**Symptoms**: Real alerts getting locked out

**Solutions**:
1. **Increase learn count**: Change from 2 to 3 passes required
2. **Decrease distance**: Reduce `lockoutMaxDistanceM` to 30m
3. **Decrease signal threshold**: Lower `lockoutMaxSignalStrength` to 4
4. **Enable Ka protection**: Prevents Ka band from being locked out

### Can't delete lockout

**Symptoms**: Lockout persists after manual delete

**Solutions**:
1. **Brake to delete**: When alert active, tap brake 3x (default)
2. **Check unlearn count**: May need multiple brake taps
3. **Use web UI**: Delete via GPS page in web interface
4. **Clear all**: Last resort - clear all lockouts

---

## Camera Alerts

### Camera alerts not working

**Symptoms**: No alert when passing known cameras

**Solutions**:
1. **Enable feature**: Settings → `cameraAlertsEnabled` = true
2. **Load database**: Upload camera CSV or sync from OSM
3. **Check GPS**: Camera alerts require GPS fix
4. **Check distance**: Default alert distance is 500m
5. **Check audio**: Enable `cameraAudioEnabled` for sound

### OSM sync fails

**Symptoms**: "Sync failed" or no cameras loaded

**Solutions**:
1. **Connect to WiFi**: OSM sync requires internet via WiFi client
2. **Check WiFi status**: Ensure connected to external network
3. **Try smaller radius**: Large areas may timeout
4. **Check coordinates**: Ensure lat/lon are in valid range

### False camera alerts

**Symptoms**: Alert where no camera exists

**Solutions**:
1. **Database outdated**: Re-sync from OSM for latest data
2. **Camera removed**: Physical camera may have been removed
3. **Adjust distance**: Reduce `cameraAlertDistanceM` to 300m

---

## OBD/Speed Issues

### Can't find OBD device

**Symptoms**: Scan shows no devices

**Solutions**:
1. **Check OBD adapter**: Ensure it's plugged in and has power LED
2. **Compatible adapter**: Must be ELM327 Bluetooth (not WiFi)
3. **Engine on**: Some adapters need engine running
4. **Scan longer**: Some adapters are slow to advertise

### OBD speed shows 0 or wrong value

**Symptoms**: Speed doesn't update or is incorrect

**Solutions**:
1. **Check vehicle compatibility**: Not all vehicles support speed PID
2. **Engine running**: Speed only works with engine on
3. **Wait for warmup**: Some ECUs need 30 seconds to report
4. **Try different adapter**: Some cheap adapters have issues

### OBD disconnects frequently

**Symptoms**: Connects but drops after a few minutes

**Solutions**:
1. **Check adapter position**: Ensure firm connection to OBD port
2. **Reduce distance**: Keep V1-Simple closer to OBD port
3. **Check interference**: V1 BLE can interfere; try different positions
4. **Check adapter quality**: Cheap clones may have weak Bluetooth

---

## Audio Problems

### No sound from device

**Symptoms**: No audio alerts or beeps

**Solutions**:
1. **Check mute**: Ensure device is not muted
2. **Check volume**: Tap display to adjust
3. **Check audio profile**: Settings → Audio
4. **Test speaker**: Use audio test in debug page

### Audio is distorted

**Symptoms**: Crackling or clipping sounds

**Solutions**:
1. **Lower volume**: High volume can cause distortion
2. **Check connections**: Internal speaker wire may be loose
3. **Power cycle**: Audio codec may need reset

---

## Performance Issues

### Web UI is slow

**Symptoms**: Pages load slowly or timeout

**Solutions**:
1. **Reduce clients**: Only connect 1-2 devices at a time
2. **Close unused pages**: Each page polls for updates
3. **Check distance**: Poor WiFi signal causes slow response
4. **Disable debug logging**: High logging rate affects performance

### Device restarting/crashing

**Symptoms**: Device reboots unexpectedly

**Solutions**:
1. **Check battery**: Low battery can cause restarts
2. **Check temperature**: Overheating triggers thermal shutdown
3. **Check for patterns**: Note what was happening when crash occurred
4. **Check debug logs**: Download logs for crash analysis
5. **Update firmware**: Latest version may have bug fixes

### High memory usage warnings

**Symptoms**: "Low memory" in debug metrics

**Solutions**:
1. **Restart device**: Clears fragmented memory
2. **Reduce lockouts**: Large lockout databases use RAM
3. **Disable unused features**: GPS, OBD, proxy mode
4. **Update firmware**: Memory optimizations in newer versions

---

## Factory Reset

### How to factory reset

**Warning**: This erases all settings, WiFi passwords, lockouts, and profiles.

**Method 1 - Via Web UI**:
1. Connect to device WiFi
2. Go to Settings page
3. Click "Factory Reset"
4. Confirm reset

**Method 2 - Via USB Serial**:
```bash
pio run -t erase -e waveshare-349
pio run -t uploadfs -e waveshare-349
pio run -t upload -e waveshare-349
```

**After reset**:
- WiFi: `V1-Simple` / `setupv1g2`
- All settings return to defaults
- Lockouts and profiles are erased

---

## Getting Help

### Collecting diagnostic info

When reporting issues, include:

1. **Firmware version**: Settings → About
2. **Debug logs**: Debug → Download Logs
3. **Debug metrics**: Debug → View Metrics
4. **Steps to reproduce**: What exactly triggers the issue
5. **Device info**: V1 version, phone model, etc.

### Debug mode

Enable detailed logging:
1. Go to Settings
2. Enable desired log categories:
   - `logAlerts` - Alert processing
   - `logBle` - Bluetooth communication
   - `logGps` - GPS data
   - `logWifi` - WiFi/HTTP activity
3. Reproduce issue
4. Download logs from Debug page

---

*Last updated: 2024*
