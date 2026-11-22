# Firmware

An Arduino sketch for the XIAO ESP32-S3 and the Adafruit 1.54" ST7789 display. The sketch is in [photo_gallery/](photo_gallery/).

## Private files

The original frame used private data, so this version has stand-ins.

| File | Here | What to do |
|---|---|---|
| `secrets.h` | not included, template is `secrets.example.h` | Copy it to `secrets.h` and fill in Wi-Fi, time zone and location |
| `birthdays.h` | not included, template is `birthdays.example.h` | Copy it to `birthdays.h` and enter your names and dates |
| `photos.h` | six sample images | Make your own with `tools/make_photos_h.py` |
| `stickers.h` | four generic sprites | Replace with your own if you like |

`secrets.h` and `birthdays.h` are in `.gitignore`, so they are never uploaded.

## Upload with the Arduino IDE

1. Install the esp32 board package by Espressif and select the board XIAO_ESP32S3.
2. In the Library Manager, install Adafruit GFX Library, Adafruit ST7735 and ST7789 Library, and JPEGDEC.
3. Copy the two `.example.h` files in `photo_gallery/` to `secrets.h` and `birthdays.h` and edit them.
4. Choose a partition scheme with an app size of about 3 MB. The sketch is about 1.1 MB with the sample photos and about 2 MB with 60 photos, which is too big for the 1.2 MB default.
5. Open `photo_gallery/photo_gallery.ino` and upload. If the board does not appear as a serial port, hold its BOOT button while plugging in USB.

## Upload with PlatformIO

```
cd firmware
pio run -t upload
```

## Your own photos

```
pip install pillow
python tools/make_photos_h.py my_photos/ photo_gallery/photos.h
```

The script crops each photo to a square, resizes it to 240x240 and saves it as a baseline JPEG at quality 80 (10 to 25 KB each). It prints the total size. Add `--focus 0.25` to keep faces that sit near the top of portrait photos.

## What it does

- Shows photos in random order, one minute each, with transitions.
- Shows the time and date at the top, switching every 10 seconds, and the weather (Open-Meteo) at the bottom right. Text colour follows the photo.
- Shows Wi-Fi signal bars at the bottom left.
- On the dates in `birthdays.h`, shows an animated full-screen greeting at power-up and every 15 minutes.
- Shows a short greeting after every other photo.
- Reconnects Wi-Fi by itself, keeps the clock and weather across restarts, and re-sends the display settings every 5 seconds.

## Button

| Action | Result |
|---|---|
| Press | next photo |
| Press twice | previous photo |
| Press three times | game |
| Hold 5 s | restart |

## Pins

| Signal | XIAO pin | GPIO |
|---|---|---|
| SCK | D8 | 7 |
| MOSI | D10 | 9 |
| MISO (SD card only) | D9 | 8 |
| TFT CS | D1 | 2 |
| DC | D2 | 3 |
| RST | D3 | 4 |
| Backlight (through R1) | D4 | 5 |
| SD CS | D5 | 6 |
| Button to GND | D0 | 1 |

## Troubleshooting

- "Missing secrets.h" or "Missing birthdays.h": copy the example files.
- No clock or weather: the ESP32-S3 only joins 2.4 GHz networks. A phone hotspot works.
- Picture upside down: change `DISPLAY_ROTATION` from 3 to 1.
- "Sketch too big": pick a partition scheme with a bigger app partition.
- Odd behaviour at power-up: GPIO3 (DC) is a strapping pin on the ESP32-S3. Check it first.
