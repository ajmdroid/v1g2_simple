"""
PlatformIO pre-script for device testing.
Adds delay after upload to allow USB CDC to re-enumerate before opening serial.
"""
import time
Import("env")

def post_upload_delay(source, target, env):
    """Wait for USB CDC device to re-enumerate after upload."""
    print("Waiting 3 seconds for USB CDC to re-enumerate...")
    time.sleep(3)

# Only apply to device environment during testing
if env.get("PIOENV") == "device":
    env.AddPostAction("upload", post_upload_delay)
