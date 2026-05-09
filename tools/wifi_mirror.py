"""
PC Screen Mirror → ESP32 ST7789 via WiFi TCP (with touch input)

Requires: pip install mss pillow pyautogui
Usage:   python wifi_mirror.py <ESP32_IP>

Protocol:
  PC → ESP32:  [0xAA] [0x55] [4 bytes LE frame_size] [JPEG data]
  ESP32 → PC:  [0xBB] [state] [x_hi] [x_lo] [y_hi] [y_lo]
"""

import io
import sys
import time
import struct
import socket
import threading
import mss
from PIL import Image

try:
    import pyautogui
    pyautogui.FAILSAFE = False
    HAS_PYAUTOGUI = True
except ImportError:
    HAS_PYAUTOGUI = False
    print("Warning: pyautogui not installed. Touch input disabled.")
    print("  Install with: pip install pyautogui")

# ── Config ──────────────────────────────────────────
SCR_W = 320
SCR_H = 240
JPEG_QUALITY = 60
TARGET_FPS = 15
TCP_PORT = 8080
TOUCH_PORT = 8082

SYNC = b'\xAA\x55'
MAX_FRAME = 128 * 1024


def touch_server(screen_w, screen_h):
    """Listen for ESP32 touch connection and control PC mouse."""
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', TOUCH_PORT))
    server.listen(1)
    print(f"Touch server listening on port {TOUCH_PORT}...")

    while True:
        conn, addr = server.accept()
        print(f"Touch connected from {addr[0]}")
        conn.settimeout(None)
        last_state = 0

        try:
            while True:
                data = conn.recv(6)
                if not data or len(data) < 6:
                    break
                if data[0] != 0xBB:
                    continue

                state = data[1]
                x = (data[2] << 8) | data[3]
                y = (data[4] << 8) | data[5]

                # Map ESP32 screen coords to PC screen coords
                pc_x = int(x * screen_w / SCR_W)
                pc_y = int(y * screen_h / SCR_H)

                if HAS_PYAUTOGUI:
                    if state == 1:
                        pyautogui.moveTo(pc_x, pc_y, _pause=False)
                        if last_state == 0:
                            pyautogui.mouseDown(pc_x, pc_y, _pause=False)
                    elif state == 0 and last_state == 1:
                        pyautogui.mouseUp(pc_x, pc_y, _pause=False)

                last_state = state

        except (ConnectionResetError, BrokenPipeError):
            pass
        finally:
            conn.close()
            print("Touch disconnected")


def main():
    if len(sys.argv) < 2:
        print("Usage: python wifi_mirror.py <ESP32_IP>")
        print("  Check serial monitor for the ESP32's IP address.")
        sys.exit(1)

    host = sys.argv[1]

    # Get screen resolution for touch mapping
    sct = mss.MSS()
    monitor = sct.monitors[0]
    screen_w = monitor['width']
    screen_h = monitor['height']

    # Start touch server in background thread
    if HAS_PYAUTOGUI:
        t = threading.Thread(target=touch_server, args=(screen_w, screen_h), daemon=True)
        t.start()

    # Connect to ESP32 frame server
    print(f"Connecting to {host}:{TCP_PORT} ...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    try:
        sock.connect((host, TCP_PORT))
    except Exception as e:
        print(f"Connection failed: {e}")
        sys.exit(1)

    sock.settimeout(None)
    print("Connected! Starting capture...")

    print(f"Capturing {screen_w}x{screen_h} → {SCR_W}x{SCR_H}")
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

            # 3) Compress to JPEG
            buf = io.BytesIO()
            pil.save(buf, format='JPEG', quality=JPEG_QUALITY)
            jpeg = buf.getvalue()

            if len(jpeg) > MAX_FRAME:
                print(f"Frame too large ({len(jpeg)} bytes), skipping")
                continue

            # 4) Send: 0xAA 0x55 + 4 bytes LE size + JPEG data
            header = SYNC + struct.pack('<I', len(jpeg))
            try:
                sock.sendall(header + jpeg)
            except (BrokenPipeError, ConnectionResetError):
                print("Connection lost")
                break

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
        sock.close()


if __name__ == '__main__':
    main()
