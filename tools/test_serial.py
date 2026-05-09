"""Minimal test: send bytes to ESP32 CDC and see if it works."""
import sys
import time
import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM21"

print(f"Opening {port}...")
ser = serial.Serial(port, timeout=1, write_timeout=5)
time.sleep(1.0)
print(f"Port open: {ser.is_open}")

for i in range(5):
    test_data = b'\xAA\x55\x01\x00\x00\x00' + bytes([i])
    print(f"[{i}] Writing {len(test_data)} bytes...")
    try:
        n = ser.write(test_data)
        print(f"[{i}] OK, wrote {n} bytes")
    except Exception as e:
        print(f"[{i}] Error: {e}")
    time.sleep(0.5)

ser.close()
print("Done")
