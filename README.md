# ESP32 Wireless Screen Mirror

Real-time PC screen mirroring to a 2.8" ST7789 IPS display (320×240), driven by ESP32-S3. Supports both WiFi (TCP) and USB CDC transport, with reverse touch input to control the PC mouse.

## Features

- **WiFi / USB Dual Transport** — Wireless TCP streaming or wired USB CDC, plug-and-play
- **Reverse Touch Control** — Touch the small screen to move your PC mouse (FT6336U capacitive touch)
- **Zero-Config WiFi** — First boot starts AP hotspot, configure WiFi in any browser at `192.168.4.1`
- **Web Config Page** — Built-in HTTP server, no app installation needed
- **HW JPEG Decode** — TJpgDec ROM library with RGB888→RGB565 conversion

## Hardware

- **Board**: ESP32-S3 (8MB PSRAM + 16MB Flash recommended)
- **Display**: 2.8" ST7789 320×240 with Intel 8080 parallel interface
- **USB OTG**: GPIO 19 (D-), GPIO 20 (D+) for USB CDC
- **Touch**: FT6336U capacitive touch controller, I2C on GPIO 12 (SDA) / GPIO 13 (SCL)
- **IO Expander**: XL9555 on I2C bus 1 (GPIO 10 SDA, GPIO 11 SCL)

## PC Requirements

```bash
pip install mss pillow pyautogui pyserial
```

## Usage

### WiFi Mode (wireless)

1. Flash the firmware and power on
2. First boot: connect to `ESP_Screen` hotspot, open `http://192.168.4.1` to configure your router's SSID/password
3. After reboot, check serial for ESP32's IP address
4. Run on PC:

```bash
python tools/wifi_mirror.py <ESP32_IP>
```

### USB Mode (wired)

```bash
python tools/screen_mirror.py
```

### Button Controls

| Button | Short Press | Long Press (3s) |
|--------|-------------|-----------------|
| IO0_1 | SD card image prev | Clear WiFi config |
| IO0_2 | SD card image next | — |
| IO0_3 | Play AVI video | — |
| IO0_4 | Start USB mirror | — |

## Protocol

```
Frame:  [0xAA] [0x55] [4 bytes LE frame_size] [JPEG data]
Touch:  [0xBB] [state] [x_hi] [x_lo] [y_hi] [y_lo]
```

## Build

- ESP-IDF 5.5.4
- Edit WiFi credentials at first boot via web config (no hardcoding needed)

```bash
idf.py build flash monitor
```

> **Note**: WiFi initialization draws significant peak current. If brownout reset occurs, use a powered USB hub or add a 470μF capacitor between 3.3V and GND.
