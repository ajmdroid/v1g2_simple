#!/usr/bin/env python3
"""
BLE Scanner - Scan for BLE devices and dump their services/characteristics.
Useful for identifying OBD adapter service UUIDs.

Usage:
    python tools/ble_scan.py                    # Scan for 10 seconds, list devices
    python tools/ble_scan.py --connect "OBDII"  # Connect to device containing "OBDII" and dump services
    python tools/ble_scan.py --address XX:XX:XX:XX:XX:XX  # Connect by address
"""

import asyncio
import argparse
import sys
from bleak import BleakScanner, BleakClient
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

# Known OBD adapter name patterns
OBD_PATTERNS = ["OBDLINK", "OBD", "ELM", "VEEPEAK", "VLINK", "IOS-VLINK", "ANDROID-VLINK", 
                "KONNWEI", "VGATE", "ICAR", "VIECAR", "ZURICH", "ZR-BT", "INNOVA", 
                "HT500", "BLCKTEC", "BLUEDRIVER"]

# Known service UUIDs
KNOWN_SERVICES = {
    "6e400001-b5a3-f393-e0a9-e50e24dcca9e": "Nordic UART Service (NUS)",
    "0000fff0-0000-1000-8000-00805f9b34fb": "FFF0 (OBDLink CX UART)",
    "0000ffe0-0000-1000-8000-00805f9b34fb": "FFE0 (Common BLE UART)",
    "00001800-0000-1000-8000-00805f9b34fb": "Generic Access",
    "00001801-0000-1000-8000-00805f9b34fb": "Generic Attribute",
    "0000180a-0000-1000-8000-00805f9b34fb": "Device Information",
    "0000180f-0000-1000-8000-00805f9b34fb": "Battery Service",
}

KNOWN_CHARACTERISTICS = {
    "6e400002-b5a3-f393-e0a9-e50e24dcca9e": "NUS RX (Write)",
    "6e400003-b5a3-f393-e0a9-e50e24dcca9e": "NUS TX (Notify)",
    "0000fff1-0000-1000-8000-00805f9b34fb": "FFF1 (Notify)",
    "0000fff2-0000-1000-8000-00805f9b34fb": "FFF2 (Write)",
    "0000ffe1-0000-1000-8000-00805f9b34fb": "FFE1 (RX/TX)",
}


def format_uuid(uuid_str: str) -> str:
    """Format UUID and add known name if available."""
    uuid_lower = uuid_str.lower()
    name = KNOWN_SERVICES.get(uuid_lower) or KNOWN_CHARACTERISTICS.get(uuid_lower)
    if name:
        return f"{uuid_str} ({name})"
    return uuid_str


def is_obd_device(name: str) -> bool:
    """Check if device name matches known OBD patterns."""
    if not name:
        return False
    upper = name.upper()
    return any(pattern in upper for pattern in OBD_PATTERNS)


def format_properties(char) -> str:
    """Format characteristic properties."""
    props = []
    if char.properties:
        props = list(char.properties)
    return ", ".join(props) if props else "none"


async def scan_devices(duration: float = 10.0) -> list[BLEDevice]:
    """Scan for BLE devices."""
    print(f"Scanning for BLE devices ({duration}s)...")
    print("-" * 60)
    
    devices_found = {}
    
    def callback(device: BLEDevice, adv: AdvertisementData):
        if device.address not in devices_found:
            devices_found[device.address] = (device, adv)
            name = device.name or "(no name)"
            obd_marker = " [OBD?]" if is_obd_device(device.name) else ""
            print(f"  Found: {name:<25} [{device.address}] RSSI:{adv.rssi:>4}dBm{obd_marker}")
            
            # Show advertised service UUIDs if any
            if adv.service_uuids:
                for uuid in adv.service_uuids:
                    print(f"         └─ Advertises: {format_uuid(uuid)}")
    
    scanner = BleakScanner(detection_callback=callback)
    await scanner.start()
    await asyncio.sleep(duration)
    await scanner.stop()
    
    print("-" * 60)
    print(f"Found {len(devices_found)} devices total")
    
    # Highlight likely OBD devices
    obd_devices = [(d, a) for d, a in devices_found.values() if is_obd_device(d.name)]
    if obd_devices:
        print("\n🔧 Likely OBD adapters:")
        for device, adv in obd_devices:
            print(f"   {device.name} [{device.address}] RSSI:{adv.rssi}dBm")
    
    return list(devices_found.values())


async def connect_and_dump(device: BLEDevice | str):
    """Connect to a device and dump all services/characteristics."""
    address = device.address if isinstance(device, BLEDevice) else device
    name = device.name if isinstance(device, BLEDevice) else address
    
    print(f"\nConnecting to {name} [{address}]...")
    
    try:
        async with BleakClient(address, timeout=20.0) as client:
            print(f"✓ Connected!")
            print(f"  MTU: {client.mtu_size}")
            print()
            
            print("=" * 70)
            print("SERVICES AND CHARACTERISTICS")
            print("=" * 70)
            
            for service in client.services:
                svc_name = format_uuid(str(service.uuid))
                print(f"\n📦 Service: {svc_name}")
                print(f"   Handle: 0x{service.handle:04X}")
                
                for char in service.characteristics:
                    char_name = format_uuid(str(char.uuid))
                    props = format_properties(char)
                    print(f"   ├─ Characteristic: {char_name}")
                    print(f"   │     Handle: 0x{char.handle:04X}, Properties: [{props}]")
                    
                    # Show descriptors
                    for desc in char.descriptors:
                        print(f"   │     └─ Descriptor: {desc.uuid} (Handle: 0x{desc.handle:04X})")
            
            print("\n" + "=" * 70)
            print("RAW SERVICE UUIDS (for code):")
            print("=" * 70)
            for service in client.services:
                print(f'  "{service.uuid}"')
                for char in service.characteristics:
                    props = list(char.properties) if char.properties else []
                    print(f'    └─ "{char.uuid}" [{", ".join(props)}]')
            
    except Exception as e:
        print(f"✗ Connection failed: {e}")
        return False
    
    return True


async def main():
    parser = argparse.ArgumentParser(description="BLE Scanner for OBD adapters")
    parser.add_argument("--connect", "-c", type=str, help="Connect to device containing this name")
    parser.add_argument("--address", "-a", type=str, help="Connect to device by address")
    parser.add_argument("--duration", "-d", type=float, default=10.0, help="Scan duration (seconds)")
    args = parser.parse_args()
    
    if args.address:
        # Direct connect by address
        await connect_and_dump(args.address)
    elif args.connect:
        # Scan then connect to matching device
        devices = await scan_devices(args.duration)
        
        # Find matching device
        pattern = args.connect.upper()
        matching = [(d, a) for d, a in devices if d.name and pattern in d.name.upper()]
        
        if not matching:
            print(f"\n✗ No device found matching '{args.connect}'")
            print("  Try running without --connect to see all devices")
            return 1
        
        if len(matching) > 1:
            print(f"\n⚠ Multiple devices match '{args.connect}':")
            for i, (d, a) in enumerate(matching):
                print(f"  {i+1}. {d.name} [{d.address}]")
            print("  Use --address to specify exact device")
            return 1
        
        device, _ = matching[0]
        await connect_and_dump(device)
    else:
        # Just scan
        await scan_devices(args.duration)
        print("\nTip: Use --connect 'NAME' or --address XX:XX:XX to dump services")
    
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
