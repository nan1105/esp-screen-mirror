"""
PC Screen Mirror → ESP32 ST7789 via USB CDC

Requires: pip install pyserial mss pillow
Usage:   python screen_mirror.py [COM_PORT]

Protocol:
  [0xAA] [0x55] [4 bytes LE frame_size] [JPEG data]
"""

import io
import sys
import time
import struct
import serial
import mss
from PIL import Image

# ── Config ──────────────────────────────────────────
SCR_W = 320
SCR_H = 240
JPEG_QUALITY = 60       # 40-70 good range; higher=better quality, larger size
TARGET_FPS = 20         # upper limit, actual depends on compress speed
BAUD = 2000000          # USB CDC doesn't use this, but pyserial requires it

SYNC = b'\xAA\x55'
MAX_FRAME = 128 * 1024


def list_ports():
    """Print available COM ports with USB info."""
    from serial.tools.list_ports import comports
    for p in comports():
        vid = f"0x{getattr(p, 'vid', 0):04X}" if getattr(p, 'vid', None) else "----"
        pid = f"0x{getattr(p, 'pid', 0):04X}" if getattr(p, 'pid', None) else "----"
        print(f"  {p.device:8s}  VID:{vid} PID:{pid}  {p.description}")


def find_esp_port():
    """Try to find the ESP32 TinyUSB CDC port (not the USB Serial/JTAG port).

    ESP32-S3 has two USB devices:
      - USB Serial/JTAG:  VID=0x303A PID=0x1001  (flashing/console, skip)
      - TinyUSB CDC:      VID=0x303A PID=0x4000..0x4007  (screen mirror)
    """
    from serial.tools.list_ports import comports
    candidates = []
    for p in comports():
        vid = getattr(p, 'vid', None)
        pid = getattr(p, 'pid', None)
        if vid == 0x303A:
            # TinyUSB CDC uses PID 0x4000-0x4007
            if pid in (0x4000, 0x4001, 0x4002, 0x4003, 0x4004, 0x4005, 0x4006, 0x4007):
                return p.device
            # USB Serial/JTAG is PID 0x1001 — skip explicitly
            if pid == 0x1001:
                continue
            candidates.append(p)
    # Fallback: first non-JTAG Espressif device
    for p in candidates:
        return p.device
    return None


def main():
    # ── Handle --list flag ──────────────────────
    if len(sys.argv) > 1 and sys.argv[1] in ('--list', '-l'):
        print("Available COM ports:")
        list_ports()
        sys.exit(0)

    # ── Find COM port ────────────────────────────
    port = sys.argv[1] if len(sys.argv) > 1 else find_esp_port()
    if not port:
        print("No ESP32 TinyUSB CDC port found.")
        print("Make sure the ESP32 is connected via USB OTG and firmware is flashed.")
        print("\nAvailable ports:")
        list_ports()
        print("\nUsage: python screen_mirror.py COM_PORT")
        sys.exit(1)

    print(f"Connecting to {port} ...")
    ser = serial.Serial(port, timeout=1, write_timeout=5)
    # Wait for USB CDC enumeration to complete
    time.sleep(1.5)
    print("Serial port ready, starting capture...")

    # ── Screen capture ───────────────────────────
    sct = mss.MSS()
    monitor = sct.monitors[0]  # 0 = all monitors combined

    print(f"Capturing {monitor['width']}x{monitor['height']} → {SCR_W}x{SCR_H}")
    print(f"Target FPS: {TARGET_FPS}, JPEG quality: {JPEG_QUALITY}")
    print("Press Ctrl+C to stop.")

    frame_count = 0
    fps_count = 0
    fps_tick = time.time()
    frame_time = 1.0 / TARGET_FPS

    try:
        while True:
            t0 = time.time()

            # 1) Capture
            img = sct.grab(monitor)
            pil = Image.frombytes('RGB', img.size, img.bgra, 'raw', 'BGRX')

            # 2) Scale
            pil = pil.resize((SCR_W, SCR_H), Image.BILINEAR)

            # 3) Compress to JPEG in memory
            buf = io.BytesIO()
            pil.save(buf, format='JPEG', quality=JPEG_QUALITY)
            jpeg = buf.getvalue()

            if len(jpeg) > MAX_FRAME:
                # reduce quality on next frame if this one was too big
                print(f"Frame too large ({len(jpeg)} bytes), skipping")
                continue

            # 4) Send: 0xAA 0x55 + 4 bytes LE size + JPEG data
            header = SYNC + struct.pack('<I', len(jpeg))
            ser.write(header)
            ser.write(jpeg)

            # 5) Stats
            frame_count += 1
            fps_count += 1
            elapsed = time.time() - fps_tick
            if elapsed >= 2.0:
                cur_fps = fps_count / elapsed
                print(f"  {frame_count:5d} frames  |  {cur_fps:.1f} fps  |  ~{len(jpeg)//1024} KB/frame")
                fps_count = 0
                fps_tick = time.time()

            # 6) Rate limiting
            t_elapsed = time.time() - t0
            if t_elapsed < frame_time:
                time.sleep(frame_time - t_elapsed)

    except KeyboardInterrupt:
        print(f"\nStopped. {frame_count} frames sent.")
    finally:
        ser.close()


if __name__ == '__main__':
    main()
