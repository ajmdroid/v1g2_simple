#!/usr/bin/env python3
"""
Flipper Zero IR Soak Test for V1 Gen2 Radar Detector
Cycles through IR signals with specific timing patterns for stress testing
"""

import serial
import time
import sys
import argparse
from datetime import datetime, timedelta

FLIPPER_PORT = '/dev/tty.usbmodemflip_Onapbeu1'
FLIPPER_BAUD = 115200

class FlipperIRTester:
    def __init__(self, port=FLIPPER_PORT, baud=FLIPPER_BAUD):
        self.port = port
        self.baud = baud
        self.ser = None
        self.signal_cache = {}
        
    def connect(self):
        """Connect to Flipper Zero via serial"""
        print(f"🔌 Connecting to Flipper on {self.port}...")
        try:
            self.ser = serial.Serial(self.port, self.baud, timeout=2)
            time.sleep(1.0)
            
            # Clear any startup banner/prompts
            if self.ser.in_waiting:
                self.ser.read(self.ser.in_waiting)
            
            print("✅ Connected!")
            return True
        except Exception as e:
            print(f"❌ Connection failed: {e}")
            return False
    
    def disconnect(self):
        """Disconnect from Flipper"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("🔌 Disconnected")
    
    def extract_signal(self, filename, signal_name):
        """Read .ir file from Flipper and extract a specific signal"""
        if signal_name in self.signal_cache:
            return self.signal_cache[signal_name]
        
        # Clear any leftover data in serial buffer
        if self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
            time.sleep(0.1)
        
        cmd = f'storage read /ext/infrared/{filename}\r\n'
        self.ser.write(cmd.encode())
        
        # Wait for file to arrive - check multiple times
        response = b''
        for attempt in range(10):
            time.sleep(0.5)
            if self.ser.in_waiting:
                response += self.ser.read(self.ser.in_waiting)
            # Stop if we see the prompt ('>:') which indicates command completed
            if b'>:' in response and response.endswith(b' '):
                break
        
        response = response.decode('utf-8', errors='ignore')
        
        # Strip Flipper command echo and headers - file content starts after "Filetype:" line
        if 'Filetype: IR signals file' in response:
            response = response.split('Filetype: IR signals file', 1)[1]
        
        # Parse the file - signals are separated by '# '
        lines = response.split('\n')
        
        # Find all signal names first for debugging
        available_signals = []
        for line in lines:
            line_stripped = line.strip()
            if line_stripped.startswith('name:'):
                name_value = line_stripped.split(':', 1)[1].strip()
                available_signals.append(name_value)
        
        # Now extract the target signal
        in_signal = False
        signal_data = {}
        current_field = None
        
        for line in lines:
            line_stripped = line.strip()
            
            # Signal separator - start of new signal block
            if line_stripped == '#':
                if in_signal and 'name' in signal_data and signal_data['name'] == signal_name:
                    break  # Found our signal, stop parsing
                in_signal = False
                signal_data = {}
                current_field = None
                continue
            
            # Look for signal name
            if line_stripped.startswith('name:'):
                name_value = line_stripped.split(':', 1)[1].strip()
                if name_value == signal_name:
                    in_signal = True
                    signal_data['name'] = name_value
                continue
            
            # If we're in the target signal, collect fields
            if in_signal:
                if line_stripped.startswith('type:'):
                    signal_data['type'] = line_stripped.split(':', 1)[1].strip()
                    current_field = 'type'
                elif line_stripped.startswith('frequency:'):
                    signal_data['frequency'] = line_stripped.split(':', 1)[1].strip()
                    current_field = 'frequency'
                elif line_stripped.startswith('duty_cycle:'):
                    signal_data['duty_cycle'] = line_stripped.split(':', 1)[1].strip()
                    current_field = 'duty_cycle'
                elif line_stripped.startswith('data:'):
                    signal_data['data'] = line_stripped.split(':', 1)[1].strip()
                    current_field = 'data'
                elif current_field == 'data' and line_stripped and not line_stripped.startswith(('#', 'name:', 'type:', 'frequency:', 'duty_cycle:')):
                    # Multi-line data continuation
                    signal_data['data'] += ' ' + line_stripped
        
        # Cache the signal if we found it
        if signal_data and 'data' in signal_data:
            self.signal_cache[signal_name] = signal_data
            return signal_data
        
        print(f"   Available signals: {', '.join(available_signals)}")
        return None
    
    def transmit(self, signal_name, filename='Radar.ir', retry=True):
        """Transmit a signal via Flipper IR"""
        signal_data = self.extract_signal(filename, signal_name)
        
        # Retry logic - if first attempt fails, wait and try once more
        if signal_data is None and retry:
            print(f"⚠️  Retrying load for {signal_name}...")
            time.sleep(0.5)
            self.signal_cache.pop(signal_name, None)  # Clear from cache
            signal_data = self.extract_signal(filename, signal_name)
        
        if signal_data is None:
            print(f"❌ Failed to load signal: {signal_name}")
            return False
        
        # Build transmit command
        signal_type = signal_data.get('type', 'RAW').upper()
        frequency = signal_data.get('frequency', '38000')
        duty_cycle = signal_data.get('duty_cycle', '0.33')
        data = signal_data.get('data', '')
        
        # Convert duty_cycle from decimal to percentage (0.33 -> 33)
        duty_pct = int(float(duty_cycle) * 100)
        
        cmd = f'ir tx {signal_type} F:{frequency} DC:{duty_pct} {data}\r\n'
        
        # Clear any pending input
        if self.ser.in_waiting:
            self.ser.read(self.ser.in_waiting)
        
        # Send command
        self.ser.write(cmd.encode())
        time.sleep(0.3)  # Brief wait for transmission
        
        # Read response
        if self.ser.in_waiting:
            response = self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
        
        print(f"📡 Transmitted: {signal_name}")
        return True
    
    def startup_sequence(self):
        """Execute the startup sequence as specified by user"""
        print("\n" + "="*70)
        print("🚀 STARTUP SEQUENCE")
        print("="*70 + "\n")
        
        # XMIT → wait 15s
        if not self.transmit('Stalker_XMIT'):
            print("⚠️  CRITICAL: Failed to load Stalker_XMIT signal, aborting test")
            return False
        print("⏳ Waiting 15s (after XMIT)...")
        time.sleep(15)
        
        # REAR → wait 15s
        if not self.transmit('Stalker_rear'):
            print("⚠️  CRITICAL: Failed to load Stalker_rear signal, aborting test")
            return False
        print("⏳ Waiting 15s (after REAR)...")
        time.sleep(15)
        
        # HOLD → wait 2min
        if not self.transmit('Stalker_Hold'):
            print("⚠️  CRITICAL: Failed to load Stalker_Hold signal, aborting test")
            return False
        print("⏳ Waiting 2 minutes (after HOLD)...")
        time.sleep(120)
        
        # GE front patterns with timing
        if not self.transmit('GE_front_hold'):
            print("⚠️  Warning: Failed GE_front_hold")
        print("⏳ Waiting 15s (after GE front)...")
        time.sleep(15)
        
        if not self.transmit('GE_front_hold'):
            print("⚠️  Warning: Failed GE_front_hold")
        print("⏳ Waiting 15s (after GE front)...")
        time.sleep(15)
        
        print("⏳ Waiting 20s...")
        time.sleep(20)
        
        if not self.transmit('GE_rear_hold'):
            print("⚠️  Warning: Failed GE_rear_hold")
        print("⏳ Waiting 15s (after GE rear)...")
        time.sleep(15)
        
        if not self.transmit('GE_front_hold'):
            print("⚠️  Warning: Failed GE_front_hold (for hold)")
        
        print("\n✅ Startup sequence complete!\n")
        return True
    
    def cycle_sequence(self):
        """Execute one cycle of the main test pattern"""
        # XMIT → wait 10s
        self.transmit('Stalker_XMIT')
        time.sleep(10)
        
        # REAR → wait 20s
        self.transmit('Stalker_rear')
        time.sleep(20)
        
        # GE front → wait 5s
        self.transmit('GE_front_hold')
        time.sleep(5)
        
        # GE rear → wait 5s
        self.transmit('GE_rear_hold')
        time.sleep(5)
        
        # HOLD → wait 5s
        self.transmit('Stalker_Hold')
        time.sleep(5)
        
        # GE rear → done
        self.transmit('GE_rear_hold')
    
    def run(self, duration_hours=1.0):
        """Run the complete soak test"""
        if not self.connect():
            return False
        
        try:
            # Startup sequence
            if not self.startup_sequence():
                print("\n❌ Startup sequence failed - aborting test")
                return False
            
            # Calculate end time
            end_time = datetime.now() + timedelta(hours=duration_hours)
            cycle_count = 0
            
            print("\n" + "="*70)
            print(f"🔄 CYCLING TEST (Duration: {duration_hours:.1f} hours)")
            print(f"   End time: {end_time.strftime('%Y-%m-%d %H:%M:%S')}")
            print("="*70 + "\n")
            
            # Main cycle loop
            while datetime.now() < end_time:
                cycle_count += 1
                elapsed = datetime.now()
                remaining = end_time - elapsed
                
                print(f"\n[Cycle {cycle_count}] Elapsed: {elapsed.strftime('%H:%M:%S')}, Remaining: {str(remaining).split('.')[0]}")
                
                self.cycle_sequence()
                
                print(f"✅ Cycle {cycle_count} complete")
            
            print("\n" + "="*70)
            print(f"🏁 SOAK TEST COMPLETE")
            print(f"   Total cycles: {cycle_count}")
            print(f"   Duration: {duration_hours:.1f} hours")
            print("="*70 + "\n")
            
            return True
            
        except KeyboardInterrupt:
            print("\n\n⚠️  Test interrupted by user")
            return False
        finally:
            self.disconnect()

def main():
    parser = argparse.ArgumentParser(description='Flipper Zero IR Soak Test')
    parser.add_argument('--duration', type=float, default=1.0,
                        help='Test duration in hours (default: 1.0)')
    parser.add_argument('--port', default=FLIPPER_PORT,
                        help=f'Flipper serial port (default: {FLIPPER_PORT})')
    
    args = parser.parse_args()
    
    print("\n" + "="*70)
    print("🧪 FLIPPER ZERO IR SOAK TEST")
    print("   Target: V1 Gen2 Radar Detector")
    print(f"   Duration: {args.duration:.1f} hours")
    print("="*70)
    
    tester = FlipperIRTester(port=args.port)
    success = tester.run(duration_hours=args.duration)
    
    sys.exit(0 if success else 1)

if __name__ == '__main__':
    main()
