#!/usr/bin/env python3
"""
OBD-II Test Script - Connect to OBDLink CX and query vehicle data.

Usage:
    python tools/obd_test.py                      # Scan and connect to first OBDLink device
    python tools/obd_test.py --address XX:XX:XX   # Connect by address
    python tools/obd_test.py --name "OBDLink CX"  # Connect by name
"""

import asyncio
import argparse
import sys
from bleak import BleakScanner, BleakClient
from bleak.backends.device import BLEDevice

# OBDLink CX UUIDs (from official docs)
UART_SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb"
NOTIFY_CHAR_UUID = "0000fff1-0000-1000-8000-00805f9b34fb"  # FFF1 - notifications (RX from device)
WRITE_CHAR_UUID = "0000fff2-0000-1000-8000-00805f9b34fb"   # FFF2 - write commands (TX to device)

# Response buffer and completion flag
response_buffer = bytearray()
response_complete = asyncio.Event()


def notification_handler(characteristic, data: bytearray):
    """Handle incoming notifications from the OBD adapter."""
    global response_buffer
    
    for byte in data:
        char = chr(byte)
        if char == '>':
            # ELM327 prompt - response complete
            response_complete.set()
            return
        elif char not in '\r\n':
            response_buffer.append(byte)


async def send_command(client: BleakClient, cmd: str, timeout: float = 2.0) -> str:
    """Send an AT/OBD command and wait for response."""
    global response_buffer
    
    # Clear buffer and event
    response_buffer.clear()
    response_complete.clear()
    
    # Send command with CR
    cmd_bytes = (cmd + "\r").encode('ascii')
    print(f"  TX: {cmd}")
    
    await client.write_gatt_char(WRITE_CHAR_UUID, cmd_bytes, response=False)
    
    # Wait for response (with '>' prompt)
    try:
        await asyncio.wait_for(response_complete.wait(), timeout)
        response = response_buffer.decode('ascii', errors='ignore')
        print(f"  RX: {response}")
        return response
    except asyncio.TimeoutError:
        print(f"  RX: (timeout)")
        return ""


async def initialize_adapter(client: BleakClient) -> bool:
    """Initialize the OBD adapter with standard AT commands."""
    print("\n--- Initializing OBD Adapter ---")
    
    # Reset
    response = await send_command(client, "ATZ", timeout=3.0)
    if "ELM" not in response.upper():
        print("  ⚠ Warning: ELM327 not confirmed in reset response")
    
    # Echo off
    await send_command(client, "ATE0")
    
    # Linefeeds off
    await send_command(client, "ATL0")
    
    # Spaces off (compact responses)
    await send_command(client, "ATS0")
    
    # Headers off
    await send_command(client, "ATH0")
    
    # Auto-detect protocol
    response = await send_command(client, "ATSP0", timeout=5.0)
    
    print("--- Adapter Initialized ---\n")
    return True


def parse_speed(response: str) -> int | None:
    """Parse speed response (410DXX -> km/h)."""
    response = response.upper()
    idx = response.find("410D")
    if idx < 0:
        return None
    
    hex_val = response[idx+4:idx+6]
    if len(hex_val) < 2:
        return None
    
    try:
        return int(hex_val, 16)
    except ValueError:
        return None


def parse_rpm(response: str) -> int | None:
    """Parse RPM response (410CXXYY -> RPM)."""
    response = response.upper()
    idx = response.find("410C")
    if idx < 0:
        return None
    
    hex_val = response[idx+4:idx+8]
    if len(hex_val) < 4:
        return None
    
    try:
        a = int(hex_val[0:2], 16)
        b = int(hex_val[2:4], 16)
        return ((a * 256) + b) // 4
    except ValueError:
        return None


def parse_coolant_temp(response: str) -> int | None:
    """Parse coolant temp response (4105XX -> degrees C)."""
    response = response.upper()
    idx = response.find("4105")
    if idx < 0:
        return None
    
    hex_val = response[idx+4:idx+6]
    if len(hex_val) < 2:
        return None
    
    try:
        return int(hex_val, 16) - 40  # Offset by 40
    except ValueError:
        return None


def parse_intake_air_temp(response: str) -> int | None:
    """Parse intake air temp response (410FXX -> degrees C)."""
    response = response.upper()
    idx = response.find("410F")
    if idx < 0:
        return None
    
    hex_val = response[idx+4:idx+6]
    if len(hex_val) < 2:
        return None
    
    try:
        return int(hex_val, 16) - 40  # Offset by 40 (same as coolant)
    except ValueError:
        return None


def parse_oil_temp(response: str) -> int | None:
    """Parse oil temp response (415CXX -> degrees C)."""
    response = response.upper()
    idx = response.find("415C")
    if idx < 0:
        return None
    
    hex_val = response[idx+4:idx+6]
    if len(hex_val) < 2:
        return None
    
    try:
        return int(hex_val, 16) - 40  # Offset by 40
    except ValueError:
        return None


def parse_vw_oil_temp(response: str) -> int | None:
    """Parse VW oil temp from Mode 22 response (62XXXX YY -> degrees C).
    VW uses various PIDs - response starts with 62 + PID echo.
    """
    response = response.upper().replace(" ", "")
    
    # Look for Mode 22 response (62 = positive response to 22)
    # Format: 62 + 2-byte PID + data
    # Common patterns: 62F40C XX XX
    
    # Try F40C (common VW oil temp PID) - 2 byte value
    idx = response.find("62F40C")
    if idx >= 0:
        hex_val = response[idx+6:idx+10]  # Get 2 bytes (4 hex chars)
        if len(hex_val) >= 4:
            try:
                raw = int(hex_val, 16)
                # VW typically uses: temp = (raw * 0.1) - 40 for some PIDs
                # Or just raw / 10 for others. Let's check both interpretations.
                # If raw is 0000, it means sensor not ready
                if raw == 0:
                    return None  # Sensor not ready/engine off
                # Try raw - 40 first (single byte equivalent in high byte)
                high_byte = (raw >> 8) & 0xFF
                if high_byte > 0:
                    return high_byte - 40
                # Try raw / 10 - some VW use this
                return int(raw * 0.1) - 40
            except ValueError:
                pass
        # Also try single byte
        hex_val = response[idx+6:idx+8]
        if len(hex_val) >= 2:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None
                return raw - 40
            except ValueError:
                pass
    
    # Try 0202 (another VW oil temp PID) - 2 byte value
    idx = response.find("620202")
    if idx >= 0:
        hex_val = response[idx+6:idx+10]
        if len(hex_val) >= 4:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None
                # VW often uses raw value / 10 - 40 or similar
                return int(raw * 0.1) - 40
            except ValueError:
                pass
    
    # Try 2001 (yet another VW PID)
    idx = response.find("622001")
    if idx >= 0:
        hex_val = response[idx+6:idx+8]
        if len(hex_val) >= 2:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None
                return raw - 40
            except ValueError:
                pass
    
    return None


def parse_vw_dsg_temp(response: str) -> int | None:
    """Parse VW DSG/transmission temp from Mode 22 response.
    DSG uses transmission ECU (7E1) with various PIDs.
    """
    response = response.upper().replace(" ", "")
    
    # Look for Mode 22 response (62 = positive response)
    # Common VW DSG temp PIDs:
    
    # Try 02B4 (common DSG oil temp) - typically 2 bytes
    idx = response.find("6202B4")
    if idx >= 0:
        hex_val = response[idx+6:idx+10]
        if len(hex_val) >= 4:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None  # Sensor not ready
                # VW DSG typically: raw * 0.1 - 40 or raw - 40
                # Try high byte first (if 2-byte value)
                high_byte = (raw >> 8) & 0xFF
                if high_byte > 0:
                    return high_byte - 40
                return int(raw * 0.1) - 40
            except ValueError:
                pass
        # Single byte fallback
        hex_val = response[idx+6:idx+8]
        if len(hex_val) >= 2:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None
                return raw - 40
            except ValueError:
                pass
    
    # Try 1940 (DSG oil temp on some models)
    idx = response.find("621940")
    if idx >= 0:
        hex_val = response[idx+6:idx+10]
        if len(hex_val) >= 4:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None
                high_byte = (raw >> 8) & 0xFF
                if high_byte > 0:
                    return high_byte - 40
                return int(raw * 0.1) - 40
            except ValueError:
                pass
    
    # Try 0190 (transmission temp on some VAG)
    idx = response.find("620190")
    if idx >= 0:
        hex_val = response[idx+6:idx+10]
        if len(hex_val) >= 4:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None
                # Some use raw / 10 directly (no offset)
                return int(raw * 0.1)
            except ValueError:
                pass
    
    # Try F446 (DQ250/DQ381 DSG temp)
    idx = response.find("62F446")
    if idx >= 0:
        hex_val = response[idx+6:idx+8]
        if len(hex_val) >= 2:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None
                return raw - 40
            except ValueError:
                pass
    
    # Try F40D (DSG temp - similar to F40C for engine oil)
    idx = response.find("62F40D")
    if idx >= 0:
        hex_val = response[idx+6:idx+10]  # Get 2 bytes (4 hex chars)
        if len(hex_val) >= 4:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None  # Sensor not ready/engine off
                high_byte = (raw >> 8) & 0xFF
                if high_byte > 0:
                    return high_byte - 40
                return int(raw * 0.1) - 40
            except ValueError:
                pass
        # Single byte fallback
        hex_val = response[idx+6:idx+8]
        if len(hex_val) >= 2:
            try:
                raw = int(hex_val, 16)
                if raw == 0:
                    return None
                return raw - 40
            except ValueError:
                pass
    
    return None


def parse_voltage(response: str) -> float | None:
    """Parse voltage response (e.g., '12.5V')."""
    response = response.strip()
    if response.endswith('V'):
        response = response[:-1]
    try:
        return float(response)
    except ValueError:
        return None


async def query_vehicle_data(client: BleakClient):
    """Query various vehicle parameters."""
    print("--- Querying Vehicle Data ---")
    
    # Check supported PIDs first
    response = await send_command(client, "0100", timeout=5.0)
    if "NO DATA" in response or "ERROR" in response:
        print("\n⚠ Vehicle may not be running or not responding")
        print("  Make sure ignition is ON (engine running or ACC mode)")
        return
    
    print(f"\nSupported PIDs: {response}")
    
    # Query Speed (PID 0x0D)
    print("\n📊 Vehicle Speed (PID 0x0D):")
    response = await send_command(client, "010D", timeout=1.0)
    speed = parse_speed(response)
    if speed is not None:
        mph = speed * 0.621371
        print(f"   ✓ Speed: {speed} km/h ({mph:.1f} mph)")
    else:
        print(f"   ✗ Could not parse speed")
    
    # Query RPM (PID 0x0C)
    print("\n📊 Engine RPM (PID 0x0C):")
    response = await send_command(client, "010C", timeout=1.0)
    rpm = parse_rpm(response)
    if rpm is not None:
        print(f"   ✓ RPM: {rpm}")
    else:
        print(f"   ✗ Could not parse RPM")
    
    # Query Coolant Temp (PID 0x05)
    print("\n📊 Coolant Temperature (PID 0x05):")
    response = await send_command(client, "0105", timeout=1.0)
    temp = parse_coolant_temp(response)
    if temp is not None:
        temp_f = (temp * 9/5) + 32
        print(f"   ✓ Coolant: {temp}°C ({temp_f:.0f}°F)")
    else:
        print(f"   ✗ Could not parse temperature")
    
    # Query Intake Air Temp (PID 0x0F)
    print("\n📊 Intake Air Temperature (PID 0x0F):")
    response = await send_command(client, "010F", timeout=1.0)
    temp = parse_intake_air_temp(response)
    if temp is not None:
        temp_f = (temp * 9/5) + 32
        print(f"   ✓ Intake Air: {temp}°C ({temp_f:.0f}°F)")
    else:
        print(f"   ✗ Could not parse intake air temp (may not be supported)")
    
    # Query Oil Temp (PID 0x5C)
    print("\n📊 Oil Temperature (PID 0x5C - Standard):")
    response = await send_command(client, "015C", timeout=1.0)
    temp = parse_oil_temp(response)
    if temp is not None:
        temp_f = (temp * 9/5) + 32
        print(f"   ✓ Oil Temp: {temp}°C ({temp_f:.0f}°F)")
    else:
        print(f"   ✗ Standard PID not supported")
    
    # VW/Audi specific: Try Mode 22 (UDS) for oil temp
    # Need to set header to ECU (typically 7E0 for engine)
    print("\n📊 Oil Temperature (VW Mode 22 - Manufacturer Specific):")
    print("   Setting up for VW extended diagnostics...")
    
    # Set CAN header to Engine ECU
    await send_command(client, "ATSH7E0", timeout=1.0)
    
    # Try various VW oil temp PIDs via Mode 22
    vw_oil_pids = [
        ("22F40C", "F40C"),    # Common VW oil temp
        ("220202", "0202"),    # EA888 engine oil temp
        ("222001", "2001"),    # Another VW variant
        ("22202F", "202F"),    # Oil temp on some models
        ("221040", "1040"),    # Oil temp alternate
    ]
    
    oil_temp_found = False
    for cmd, pid_name in vw_oil_pids:
        response = await send_command(client, cmd, timeout=1.0)
        if "NO DATA" not in response and "ERROR" not in response and "?" not in response:
            # Check for 7F (negative response) - service not supported
            if response.startswith("7F"):
                continue
            print(f"   Raw response for {pid_name}: {response}")
            # Check for 0000 response (sensor exists but cold/off)
            if "0000" in response and response.startswith("62"):
                print(f"   ✓ VW Oil Temp ({pid_name}): Sensor responding - 0 (engine cold or off)")
                oil_temp_found = True
                break
            temp = parse_vw_oil_temp(response)
            if temp is not None:
                temp_f = (temp * 9/5) + 32
                print(f"   ✓ VW Oil Temp ({pid_name}): {temp}°C ({temp_f:.0f}°F)")
                oil_temp_found = True
                break
    
    if not oil_temp_found:
        print(f"   ✗ VW oil temp PIDs not responding")
        print(f"   (May need different header or PID for this specific ECU)")
    
    # VW DSG Transmission Temperature
    print("\n📊 DSG Transmission Temperature (VW Mode 22):")
    print("   Setting header to Transmission ECU (7E1)...")
    
    # Set CAN header to Transmission ECU (DSG)
    await send_command(client, "ATSH7E1", timeout=1.0)
    
    # Try various VW DSG temp PIDs via Mode 22
    vw_dsg_pids = [
        ("2202B4", "02B4"),    # Common DSG oil temp
        ("221940", "1940"),    # DSG oil temp alternate
        ("220190", "0190"),    # Transmission temp
        ("22F446", "F446"),    # DQ250/DQ381 DSG temp
        ("22F40D", "F40D"),    # Gearbox temp on some VAG
    ]
    
    dsg_temp_found = False
    for cmd, pid_name in vw_dsg_pids:
        response = await send_command(client, cmd, timeout=1.0)
        if "NO DATA" not in response and "ERROR" not in response and "?" not in response:
            # Check for 7F (negative response) - service not supported
            if response.startswith("7F"):
                continue
            print(f"   Raw response for {pid_name}: {response}")
            # Check for 0000 response (sensor exists but cold/off)
            if "0000" in response and response.startswith("62"):
                print(f"   ✓ VW DSG ({pid_name}): Sensor responding - 0 (engine/trans cold or off)")
                dsg_temp_found = True
                break
            temp = parse_vw_dsg_temp(response)
            if temp is not None:
                temp_f = (temp * 9/5) + 32
                print(f"   ✓ VW DSG Temp ({pid_name}): {temp}°C ({temp_f:.0f}°F)")
                dsg_temp_found = True
                break
    
    if not dsg_temp_found:
        print(f"   ✗ VW DSG temp PIDs not responding")
        print(f"   (DSG may need ignition ON or different PID for MK7 GTI)")
    
    # Reset headers
    await send_command(client, "ATH0", timeout=0.5)
    await send_command(client, "ATSH", timeout=0.5)  # Reset to default

    # Query Battery Voltage (AT RV)
    print("\n📊 Battery Voltage (AT RV):")
    response = await send_command(client, "ATRV", timeout=1.0)
    voltage = parse_voltage(response)
    if voltage is not None:
        print(f"   ✓ Voltage: {voltage}V")
    else:
        print(f"   ✗ Could not parse voltage (response: {response})")
    
    print("\n--- Query Complete ---")


async def continuous_speed_monitor(client: BleakClient, duration: int = 30):
    """Continuously monitor speed for a given duration."""
    print(f"\n--- Continuous Speed Monitor ({duration}s) ---")
    print("Press Ctrl+C to stop\n")
    
    start_time = asyncio.get_event_loop().time()
    
    try:
        while asyncio.get_event_loop().time() - start_time < duration:
            response = await send_command(client, "010D", timeout=1.0)
            speed = parse_speed(response)
            if speed is not None:
                mph = speed * 0.621371
                print(f"  Speed: {speed:3d} km/h ({mph:5.1f} mph)")
            else:
                print(f"  Speed: --")
            
            await asyncio.sleep(0.5)  # Poll every 500ms
    except KeyboardInterrupt:
        print("\nStopped by user")
    
    print("--- Monitor Complete ---")


async def find_obd_device(name_filter: str | None = None) -> BLEDevice | None:
    """Scan for OBD devices."""
    print("Scanning for BLE devices...")
    
    devices = await BleakScanner.discover(timeout=10.0)
    
    obd_devices = []
    for device in devices:
        if device.name:
            name_upper = device.name.upper()
            is_obd = any(p in name_upper for p in ["OBDLINK", "OBD", "ELM", "VEEPEAK", "VLINK"])
            
            if name_filter:
                if name_filter.upper() in name_upper:
                    return device
            elif is_obd:
                obd_devices.append(device)
                print(f"  Found: {device.name} [{device.address}]")
    
    if obd_devices:
        # Return first one found
        return obd_devices[0]
    
    return None


async def main():
    parser = argparse.ArgumentParser(description="OBD-II Test Script for OBDLink CX")
    parser.add_argument("--address", "-a", type=str, help="Connect by BLE address")
    parser.add_argument("--name", "-n", type=str, help="Connect by device name")
    parser.add_argument("--monitor", "-m", action="store_true", help="Continuous speed monitoring")
    parser.add_argument("--duration", "-d", type=int, default=30, help="Monitor duration in seconds")
    args = parser.parse_args()
    
    # Find or specify device
    if args.address:
        address = args.address
        print(f"Using specified address: {address}")
    else:
        device = await find_obd_device(args.name)
        if not device:
            print("✗ No OBD device found!")
            print("  Make sure your OBDLink CX is powered on (plugged into car with ignition on)")
            return 1
        
        address = device.address
        print(f"\n✓ Found: {device.name} [{address}]")
    
    # Connect
    print(f"\nConnecting to {address}...")
    
    async with BleakClient(address, timeout=30.0) as client:
        print(f"✓ Connected!")
        print(f"  MTU: {client.mtu_size}")
        
        # OBDLink CX requires pairing/bonding for encrypted characteristics
        # Try to pair first - this triggers the bonding process
        print(f"\nPairing with device (PIN: 123456)...")
        try:
            # On macOS, pair() initiates the pairing process
            # The OS handles PIN entry automatically for numeric comparison
            paired = await client.pair(protection_level=2)  # 2 = encrypted with MITM protection
            if paired:
                print(f"✓ Paired successfully!")
            else:
                print(f"  Pairing returned False, but may still work...")
        except NotImplementedError:
            # Some backends don't implement pair()
            print(f"  Pairing not implemented on this platform, continuing...")
        except Exception as e:
            print(f"  Pairing attempt: {e}")
            print(f"  Continuing anyway - encryption may already be established...")
        
        # Small delay after pairing
        await asyncio.sleep(0.5)
        
        # Subscribe to notifications (FFF1)
        print(f"\nSubscribing to notifications (FFF1)...")
        
        try:
            await client.start_notify(NOTIFY_CHAR_UUID, notification_handler)
            print(f"✓ Subscribed!")
        except Exception as e:
            print(f"✗ Failed to subscribe: {e}")
            print("\n  The device requires encryption. Troubleshooting:")
            print("  1. Make sure the CX was powered on less than 5 minutes ago")
            print("     (bonding window is only first 5 minutes after power on)")
            print("  2. Try unplugging and re-plugging the CX, then run this script immediately")
            print("  3. If using macOS, try: blueutil --unpair <address> then retry")
            return 1
        
        # Initialize adapter
        if not await initialize_adapter(client):
            print("✗ Failed to initialize adapter")
            return 1
        
        # Query vehicle data
        await query_vehicle_data(client)
        
        # Optional continuous monitoring
        if args.monitor:
            await continuous_speed_monitor(client, args.duration)
        
        # Cleanup
        await client.stop_notify(NOTIFY_CHAR_UUID)
    
    print("\n✓ Disconnected")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
