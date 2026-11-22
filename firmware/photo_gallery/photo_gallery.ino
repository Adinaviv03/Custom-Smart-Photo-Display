// Photo gallery for XIAO ESP32S3 + Adafruit 1.54" 240x240 ST7789 display
//  - Photos in flash (photos.h), shuffled, 1 minute each, with transition effects
//  - Time and date alternate across the top (10 s each), weather icon + temperature (bottom-right)
//  - Birthday greeting on the dates in BIRTHDAYS (at power-up and every 15 minutes on the quarter hour),
//    with waving rainbow letters and floating hearts and sparkles
//  - Time-of-day greeting after every other photo swap, outlined text, no background box
//  - Wi-Fi signal bars (bottom-left)
//  - Button: press = next photo, double press = previous photo, triple press = game, hold 5 s = restart. Auto-advance is always on.
//  - The display's settings are re-sent every few seconds, so a glitch that inverts it fixes itself
//  - Wi-Fi is watched: a lost connection is retried, the radio reset and, as a last resort, restarted.
//    The clock and weather are kept across restarts, and a failed weather update is retried soon.
//
// Libraries: Adafruit GFX, Adafruit ST7735 and ST7789 Library, JPEGDEC (Larry Bank)

#include <SPI.h>
#include <SD.h>                      // part of the ESP32 core (not an extra download)
#include <esp_system.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <JPEGDEC.h>
#include "photos.h"
#include "stickers.h"

// ---------- Settings you may want to change ----------
// Wi-Fi networks, time zone and location live in secrets.h (NOT in the repository).
// Copy secrets.example.h to secrets.h and fill it in. Wi-Fi is only used for the clock and the weather.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing secrets.h: copy secrets.example.h to secrets.h and edit it (Wi-Fi, time zone, location)."
#endif

const unsigned long PHOTO_MS           = 1UL * 60UL * 1000UL;   // 1 minute per photo
const unsigned long GREETING_MS        = 10000;                  // greeting stays 10 s
const unsigned long BDAY_GREETING_MS   = 15000;                  // the birthday message stays 15 s
const unsigned long WEATHER_REFRESH_MS = 15UL * 60UL * 1000UL;   // weather update every 15 min
// If a weather update fails, retry after these gaps (the last one repeats)
const unsigned long WEATHER_RETRY_MS[] = {60000UL, 120000UL, 300000UL, 600000UL, 900000UL};
const bool WIFI_POWER_SAVE = false;                              // false = Wi-Fi radio always awake: fewer dropped connections, a little more power (fine on USB)
const unsigned long WIFI_RADIO_RESET_MS = 5UL * 60UL * 1000UL;   // Wi-Fi down this long: switch the radio off and on
const unsigned long WIFI_RESTART_MS     = 6UL * 60UL * 60UL * 1000UL;  // Wi-Fi down this long: restart (clock and weather are kept)
const long WEATHER_STALE_S = 6L * 3600L;                        // weather older than this (by the clock) is hidden
const int DISPLAY_ROTATION = 3;     // 0 = portrait (old), 3 = landscape, as the frame sits in its case. If the picture is upside down, use 1.
const unsigned long GAME_IDLE_MS = 15000;                        // game: back to the photos after this long without a press
const unsigned long DEBOUNCE_MS        = 40;
const unsigned long DOUBLE_PRESS_MS    = 400;                    // two presses within this = back to the previous photo
const unsigned long DISPLAY_REFRESH_MS = 5000;                   // re-send the display's settings this often
const int GREETING_Y = 80;                                       // greeting baseline (pixels from top)
const int FX_DELAY   = 8;
const unsigned long RESET_HOLD_MS = 5000;   // hold the button this long to restart                                        // ms between transition steps (bigger = slower)
const unsigned long CORNER_SWAP_MS = 10000;                       // top-left alternates time / date every 10 s
const unsigned long LOADING_TIMEOUT_MS = 30000;                   // loading screen: at most this long, then the frame starts anyway
const unsigned long LOADING_MIN_MS     = 2500;                    // ...and at least this long, so it never just flashes

// Birthdays: month, day, message. Plain text only (the display font has no emoji).
// The message is word-wrapped to 2-3 lines and shown for GREETING_MS.
const int BDAY_MAX_LINES = 3, BDAY_LINE_LEN = 32;    // up to 3 lines of 31 characters (declared here for the auto-generated prototypes)
const unsigned long BDAY_FRAME_MS = 70;               // birthday effects: about 14 frames a second
const int BDAY_EVERY_MIN = 15;                        // the birthday message repeats this often, on the clock (must divide 60)
const int BDAY_WAVE = 2;                              // how far the letters bob up and down (pixels)
const int N_PARTS = 24;                               // floating hearts and sparkles (over the whole screen)
struct Birthday { uint8_t month, day; const char *msg; };
// The list of birthdays (month, day, message) lives in birthdays.h (NOT in the repository).
// Copy birthdays.example.h to birthdays.h and edit it.
#if __has_include("birthdays.h")
#include "birthdays.h"
#else
#error "Missing birthdays.h: copy birthdays.example.h to birthdays.h and edit it."
#endif
// -----------------------------------------------------

// Pins (match docs/wiring.md and the carrier PCB). SPI uses the default XIAO pins:
// SCK D8, MISO D9, MOSI D10 (MISO is only needed by the SD card).
#define TFT_CS   D1
#define TFT_DC   D2
#define TFT_RST  D3
#define BL_PIN   D4   // backlight (Lite); the breakout pulls it high, so full brightness if left alone
#define SD_CS    D5
#define BTN_PIN  D0

// More stack for setup()/loop() (the default 8 KB is tight with the Wi-Fi, JPEG and text code). It must stay
// below all the constants: this macro defines a function, and the IDE puts its auto-generated prototypes above the first function.
#if defined(SET_LOOP_TASK_STACK_SIZE)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
JPEGDEC jpeg;
WiFiMulti wifiMulti;

uint16_t *photoBuf = nullptr;      // clean copy of the current photo (240x240 RGB565)

int order[NUM_PHOTOS];
int pos = 0;
unsigned long lastChange = 0;
int lastFx = -1;

int swapCount = 0;                           // photo swaps so far: a greeting follows every other one
bool greetPending = false;
int lastMinute = -1;
unsigned long greetingUntil = 0;
int gX = 0, gY = 0, gW = 0, gH = 0;          // greeting area (to erase later)
bool bdayPending = true;                     // show the birthday message at power-up, as soon as the clock is known
int lastBdaySlot = -1;                       // the quarter hour (of the year) the birthday message was last checked for
bool msgFull = true;                         // the animated message covers the whole screen (birthday) or just a block (greeting)
int msgTheme = 0;                            // 0 birthday, 1 morning, 2 afternoon, 3 evening, 4 night
int partTop = 0, partBottom = 240;           // rows the floating hearts and sparkles live in
bool bdayAnimating = false;                  // the birthday message is on screen and being animated
unsigned long bdayStart = 0, lastBdayFrame = 0;
char bLines[BDAY_MAX_LINES][BDAY_LINE_LEN];  // the wrapped birthday message
int bN = 0, bBlockTop = 0, bBlockBottom = 0;  // number of lines and the rows the message block covers
const GFXfont *bLineFont[BDAY_MAX_LINES];    // each line gets the biggest font that fits the width
int bLineR[BDAY_MAX_LINES], bLineAsc[BDAY_MAX_LINES], bLineH[BDAY_MAX_LINES], bLineTop[BDAY_MAX_LINES], bLineLetters[BDAY_MAX_LINES];
float partX[N_PARTS], partY[N_PARTS], partSpeed[N_PARTS], partPhase[N_PARTS];
bool showDate = false;                       // top-left corner: false = time, true = date
unsigned long lastCornerSwap = 0;

bool weatherOk = false;
int wTemp = 0, wCode = 0;
bool wDay = true;
uint32_t weatherIpCache = 0;                 // the last address that worked for the weather server
time_t weatherEpoch = 0;                     // when the weather was last fetched (wall-clock time)
unsigned long lastWeatherOk = 0, lastWifiTry = 0;
unsigned long weatherNextTry = 0;            // millis() of the next weather attempt
int weatherFails = 0;                        // weather attempts that failed in a row
unsigned long wifiDownSince = 0, lastNtpKick = 0, lastRtcSave = 0;
int wifiAttempts = 0;
bool radioResetDone = false;

// Kept in RTC memory: these survive a restart (not a power cut), so a restart does not blank the clock
const uint32_t RTC_MAGIC = 0xB1A7D0C6;
RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR char rtcWhy[28] = "";                     // why the last software restart was asked for
RTC_DATA_ATTR uint32_t rtcRestarts = 0;                 // restarts since the last power-up
char bootReason[40] = "";                               // shown in small print on the loading screen after a restart
RTC_DATA_ATTR time_t rtcTime = 0, rtcWeatherAt = 0;
RTC_DATA_ATTR int rtcTemp = 0, rtcCode = 0;
RTC_DATA_ATTR bool rtcDay = true;

// button state
bool btnRaw = false, btnStable = false;
int wifiLevel = -1;                          // Wi-Fi icon as drawn: -1 = not connected, 1..4 = bars
bool rssiValid = false;
float rssiAvg = -100;                        // smoothed signal strength (dBm)
unsigned long btnChangedAt = 0, downAt = 0, lastReleaseAt = 0;
int clickCount = 0;                          // presses in the current series: 1 = next photo, 2 = back, 3 = game
unsigned long lastDisplayRefresh = 0;

const int HIST_MAX = 32;
int history[HIST_MAX];                       // photos shown, oldest first (for the back button)
int histLen = 0, histPos = -1;               // histPos = the entry that is on screen

// ---------------- Screen helpers ----------------
void blit(int x, int y, int w, int h, const uint16_t *buf) {
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  tft.writePixels((uint16_t *)buf, (uint32_t)w * h);
  tft.endWrite();
}

void clampRect(int &x, int &y, int &w, int &h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > 240) w = 240 - x;
  if (y + h > 240) h = 240 - y;
}

// Draw a rectangle of the clean photo onto the screen
void restoreRect(int x, int y, int w, int h) {
  clampRect(x, y, w, h);
  if (w <= 0 || h <= 0) return;
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  for (int yy = y; yy < y + h; yy++)
    tft.writePixels(photoBuf + yy * 240 + x, w);
  tft.endWrite();
}

// Off-screen patch: starts as a copy of the clean photo behind it
GFXcanvas16 *beginPatch(int &rx, int &ry, int &rw, int &rh) {
  clampRect(rx, ry, rw, rh);
  if (rw <= 0 || rh <= 0) return nullptr;
  GFXcanvas16 *cv = new GFXcanvas16(rw, rh);
  uint16_t *b = cv->getBuffer();
  if (!b) { delete cv; return nullptr; }
  for (int yy = 0; yy < rh; yy++)
    memcpy(b + yy * rw, photoBuf + (ry + yy) * 240 + rx, rw * 2);
  return cv;
}

void endPatch(GFXcanvas16 *cv, int rx, int ry, int rw, int rh) {
  blit(rx, ry, rw, rh, cv->getBuffer());
  delete cv;
}

void measure(const char *s, const GFXfont *f, int &bx, int &bw, int &by, int &bh) {
  int16_t x1, y1; uint16_t w, h;
  tft.setTextWrap(false);                 // measure the true width, no line wrapping
  tft.setFont(f);
  tft.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  tft.setFont();
  bx = x1; bw = w; by = y1; bh = h;
}

// Text with an outline in any two colours, drawn onto any Adafruit_GFX surface
void textOutlinedC(Adafruit_GFX &g, const char *s, int x, int y, const GFXfont *f, uint16_t fill, uint16_t outline, int r) {
  g.setFont(f);
  g.setTextSize(1);
  g.setTextWrap(false);
  g.setTextColor(outline);
  for (int dx = -r; dx <= r; dx++)
    for (int dy = -r; dy <= r; dy++)
      if ((dx || dy) && dx * dx + dy * dy <= r * r + 1) {
        g.setCursor(x + dx, y + dy);
        g.print(s);
      }
  g.setTextColor(fill);
  g.setCursor(x, y);
  g.print(s);
  g.setFont();
}

// Coloured text with a black outline
void textOutlined(Adafruit_GFX &g, const char *s, int x, int y, const GFXfont *f, uint16_t color, int r) {
  textOutlinedC(g, s, x, y, f, color, ST77XX_BLACK, r);
}

// Is the photo behind this rectangle light? Used to pick dark or light text, so it always stands out.
bool photoIsLight(int x, int y, int w, int h) {
  long sum = 0;
  int n = 0;
  for (int yy = y; yy < y + h && yy < 240; yy += 4)
    for (int xx = x; xx < x + w && xx < 240; xx += 4) {
      uint16_t c = photoBuf[yy * 240 + xx];
      int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
      sum += (r * 77 + g * 150 + b * 29) >> 8;            // brightness, 0..255
      n++;
    }
  return n && sum / n > 140;
}

// ---------------- Weather icons ----------------
void cloudShape(Adafruit_GFX &g, int x, int y, uint16_t c, int K) {   // K = size multiplier (1 = 26 px icon)
  g.fillCircle(x + 7 * K, y + 9 * K, 5 * K, c);
  g.fillCircle(x + 13 * K, y + 6 * K, 6 * K, c);
  g.fillCircle(x + 19 * K, y + 9 * K, 5 * K, c);
  g.fillRect(x + 7 * K, y + 9 * K, 13 * K, 5 * K, c);
}

// kind: 0 sun, 1 moon, 2 partly cloudy, 3 cloud, 4 fog, 5 rain, 6 snow, 7 storm
int iconKind(int code, bool day) {
  if (code <= 1) return day ? 0 : 1;
  if (code == 2) return day ? 2 : 3;
  if (code == 3) return 3;
  if (code == 45 || code == 48) return 4;
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return 5;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 6;
  if (code >= 95) return 7;
  return 3;
}

void thickLine(Adafruit_GFX &g, int x1, int y1, int x2, int y2, uint16_t c, int K) {
  for (int d = 0; d < K; d++) { g.drawLine(x1 + d, y1, x2 + d, y2, c); g.drawLine(x1, y1 + d, x2, y2 + d, c); }
}

void iconShape(Adafruit_GFX &g, int kind, int x, int y, bool sil, int K) {
  uint16_t YEL  = sil ? ST77XX_BLACK : ST77XX_YELLOW;
  uint16_t MOON = sil ? ST77XX_BLACK : 0xFFF9;
  uint16_t GRY  = sil ? ST77XX_BLACK : 0xC618;
  uint16_t DGR  = sil ? ST77XX_BLACK : 0x8410;
  uint16_t BLU  = sil ? ST77XX_BLACK : 0x5DFF;
  uint16_t WHT  = sil ? ST77XX_BLACK : ST77XX_WHITE;
  switch (kind) {
    case 0: {                                    // sun
      int cx = x + 13 * K, cy = y + 13 * K;
      g.fillCircle(cx, cy, 6 * K, YEL);
      for (int k = 0; k < 8; k++) {
        float a = k * PI / 4.0;
        thickLine(g, cx + cosf(a) * 9 * K, cy + sinf(a) * 9 * K, cx + cosf(a) * 12 * K, cy + sinf(a) * 12 * K, YEL, K);
      }
      break;
    }
    case 1: {                                    // moon (crescent)
      for (int yy = 0; yy < 26 * K; yy++)
        for (int xx = 0; xx < 26 * K; xx++) {
          float a = (xx + 0.5f) / K - 13, b = (yy + 0.5f) / K - 13, c2 = (xx + 0.5f) / K - 18, d = (yy + 0.5f) / K - 10;
          if (a * a + b * b <= 81 && c2 * c2 + d * d > 56) g.drawPixel(x + xx + K, y + yy, MOON);
        }
      break;
    }
    case 2: {                                    // partly cloudy
      g.fillCircle(x + 9 * K, y + 9 * K, 5 * K, YEL);
      cloudShape(g, x + 3 * K, y + 9 * K, GRY, K);
      break;
    }
    case 3: {                                    // cloud
      cloudShape(g, x + K, y + 6 * K, GRY, K);
      break;
    }
    case 4: {                                    // fog
      cloudShape(g, x + K, y + K, GRY, K);       // a cloud with mist under it (not bare lines)
      g.fillRect(x + 2 * K, y + 17 * K, 22 * K, 2 * K, GRY);
      g.fillRect(x + 5 * K, y + 22 * K, 16 * K, 2 * K, GRY);
      break;
    }
    case 5: {                                    // rain
      cloudShape(g, x + K, y + K, GRY, K);
      for (int i = 0; i < 3; i++) {
        int dx = x + 8 * K + i * 6 * K;
        thickLine(g, dx, y + 17 * K, dx - 2 * K, y + 23 * K, BLU, K);
      }
      break;
    }
    case 6: {                                    // snow
      cloudShape(g, x + K, y + K, GRY, K);
      g.fillCircle(x + 8 * K, y + 20 * K, 2 * K, WHT);
      g.fillCircle(x + 14 * K, y + 23 * K, 2 * K, WHT);
      g.fillCircle(x + 20 * K, y + 20 * K, 2 * K, WHT);
      break;
    }
    case 7: {                                    // thunderstorm
      cloudShape(g, x + K, y + K, DGR, K);
      g.fillTriangle(x + 15 * K, y + 14 * K, x + 9 * K, y + 22 * K, x + 14 * K, y + 22 * K, YEL);
      g.fillTriangle(x + 14 * K, y + 19 * K, x + 20 * K, y + 19 * K, x + 12 * K, y + 26 * K, YEL);
      break;
    }
  }
}

void drawWeatherIcon(Adafruit_GFX &g, int code, bool day, int x, int y, int K) {
  int k = iconKind(code, day);
  for (int dx = -K; dx <= K; dx++)
    for (int dy = -K; dy <= K; dy++)
      if ((dx || dy) && dx * dx + dy * dy <= K * K + 1) iconShape(g, k, x + dx, y + dy, true, K);   // black outline
  iconShape(g, k, x, y, false, K);
}

// ---------------- Overlays (built off-screen, then pushed in one go) ----------------
void drawTime() {                       // top of the screen: the time or the date, as big as fits, centred
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  char buf[16];
  if (showDate) {
    char wd[12];
    strftime(wd, sizeof(wd), "%a, %b", &t);              // e.g. "Wed, Nov"
    snprintf(buf, sizeof(buf), "%s %d", wd, t.tm_mday);  // e.g. "Wed, Nov 18"
  } else {
    strftime(buf, sizeof(buf), "%I:%M %p", &t);
  }
  const char *s = (!showDate && buf[0] == '0') ? buf + 1 : buf;
  // The biggest font that fits the width: the time fits the largest one, the date the next one down.
  const GFXfont *fonts[3] = {&FreeSansBold24pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b};
  int pick = 2, bx, bw, by, bh;
  for (int i = 0; i < 3; i++) {
    measure(s, fonts[i], bx, bw, by, bh);
    if (bw <= 232) { pick = i; break; }
  }
  measure(s, fonts[pick], bx, bw, by, bh);
  int cx, cw, cy, ch;
  measure("H", fonts[pick], cx, cw, cy, ch);             // cy = height of a capital above the baseline (negative)
  int baseline = 20 - cy / 2;                            // capitals centred on y = 20
  int rx = 0, ry = 0, rw = 240, rh = 48;                 // a band across the whole top of the screen
  GFXcanvas16 *cv = beginPatch(rx, ry, rw, rh);
  if (!cv) return;
  bool light = photoIsLight(0, 0, 240, 48);               // dark text on a light photo, light text on a dark one
  textOutlinedC(*cv, s, (240 - bw) / 2 - bx - rx, baseline - ry, fonts[pick], light ? ST77XX_BLACK : ST77XX_WHITE, light ? ST77XX_WHITE : ST77XX_BLACK, 2);
  endPatch(cv, rx, ry, rw, rh);
  lastMinute = t.tm_min;
}

void drawWeather() {                       // bottom-right: a big icon and temperature, text that suits the photo
  int rx = 96, ry = 184, rw = 144, rh = 56;
  GFXcanvas16 *cv = beginPatch(rx, ry, rw, rh);
  if (!cv) return;
  if (weatherOk) {
    bool light = photoIsLight(rx, ry, rw, rh);
    uint16_t fill = light ? ST77XX_BLACK : ST77XX_WHITE, edge = light ? ST77XX_WHITE : ST77XX_BLACK;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", wTemp);
    const GFXfont *f = &FreeSansBold24pt7b;
    int bx, bw, by, bh;
    measure(buf, f, bx, bw, by, bh);
    int endX = 234, base = 232;
    int textLeft = endX - 16 - bw;                           // leave room for the degree ring
    textOutlinedC(*cv, buf, textLeft - bx - rx, base - ry, f, fill, edge, 2);
    int rcx = endX - 6 - rx, rcy = base - 27 - ry;           // degree ring, level with the top of the digits
    cv->drawCircle(rcx, rcy, 6, edge);
    cv->drawCircle(rcx, rcy, 4, edge);
    cv->drawCircle(rcx, rcy, 5, fill);
    drawWeatherIcon(*cv, wCode, wDay, textLeft - 8 - 52 - rx, 186 - ry, 2);   // the icon at double size (52 px)
  }
  endPatch(cv, rx, ry, rw, rh);
}

// ---------------- Wi-Fi signal bars (bottom-left) ----------------
// -1 = not connected, otherwise 1..4 bars from the smoothed signal strength.
int computeWifiLevel() {
  if (WiFi.status() != WL_CONNECTED) { rssiValid = false; return -1; }
  float r = WiFi.RSSI();
  rssiAvg = rssiValid ? 0.85f * rssiAvg + 0.15f * r : r;   // smooth out the jitter
  rssiValid = true;
  if (rssiAvg >= -55) return 4;
  if (rssiAvg >= -65) return 3;
  if (rssiAvg >= -75) return 2;
  return 1;
}

void drawWifi() {
  int rx = 0, ry = 204, rw = 100, rh = 36;
  GFXcanvas16 *cv = beginPatch(rx, ry, rw, rh);
  if (!cv) return;
  const int bw = 4, gap = 2;
  const int heights[4] = {5, 9, 13, 17};
  int x0 = 8 - rx, base = 232 - ry;                        // bars grow upward from y = 232
  for (int i = 0; i < 4; i++)                              // black outline, same look as the other overlays
    cv->fillRect(x0 + i * (bw + gap) - 1, base - heights[i] - 1, bw + 2, heights[i] + 2, ST77XX_BLACK);
  for (int i = 0; i < 4; i++)                              // white = signal, grey = unused bar
    cv->fillRect(x0 + i * (bw + gap), base - heights[i], bw, heights[i], (i < wifiLevel) ? ST77XX_WHITE : 0x8410);
  if (wifiLevel < 0) {                                     // not connected: red slash across the bars
    for (int d = -1; d <= 1; d++) cv->drawLine(x0 - 3 + d, base + 1, x0 + 25 + d, base - 19, ST77XX_BLACK);
    cv->drawLine(x0 - 3, base + 1, x0 + 25, base - 19, ST77XX_RED);
    cv->drawLine(x0 - 3, base, x0 + 25, base - 20, ST77XX_RED);
  }
  endPatch(cv, rx, ry, rw, rh);
}

void updateWifiIcon() {                                    // called once a second
  static int pending = -2, count = 0;
  int lvl = computeWifiLevel();
  if (lvl == wifiLevel) { pending = -2; return; }
  if (lvl != pending) { pending = lvl; count = 0; }
  if (lvl < 0 || ++count >= 3) {                           // a disconnect shows at once, a new bar level must hold 3 s
    wifiLevel = lvl;
    pending = -2;
    drawWifi();
  }
}

void drawOverlays() {
  drawTime();
  drawWeather();
  drawWifi();
}

// ---------------- Transition effects (reveal the new photo over the old one) ----------------
void fxWipe(int dir) {                    // 0 left->right, 1 right->left, 2 top->bottom, 3 bottom->top
  const int S = 8;
  for (int i = 0; i < 240; i += S) {
    if (dir == 0) restoreRect(i, 0, S, 240);
    else if (dir == 1) restoreRect(240 - S - i, 0, S, 240);
    else if (dir == 2) restoreRect(0, i, 240, S);
    else restoreRect(0, 240 - S - i, 240, S);
    delay(FX_DELAY);
  }
}

void fxCurtains() {                       // opens from the middle like theatre curtains
  const int S = 8;
  for (int i = 0; i < 120; i += S) {
    restoreRect(120 - S - i, 0, S, 240);
    restoreRect(120 + i, 0, S, 240);
    delay(FX_DELAY + 4);
  }
}

void fxBlinds() {                         // venetian blinds
  const int SL = 20;
  for (int k = 0; k < SL; k += 2) {
    for (int y = 0; y < 240; y += SL) restoreRect(0, y + k, 240, 2);
    delay(FX_DELAY * 3);
  }
}

void fxShape(bool diamond) {              // circle (iris) or diamond growing from the centre
  int prev[240];
  for (int y = 0; y < 240; y++) prev[y] = 0;
  int maxR = diamond ? 250 : 190;
  for (int r = 10; r <= maxR; r += 10) {
    for (int y = 0; y < 240; y++) {
      int dy = y - 120, w;
      if (diamond) w = r - abs(dy);
      else {
        long v = (long)r * r - (long)dy * dy;
        w = (v > 0) ? (int)sqrtf((float)v) : -1;
      }
      if (w <= 0) continue;
      if (w > 120) w = 120;
      if (w > prev[y]) {
        restoreRect(120 - w, y, w - prev[y], 1);
        restoreRect(120 + prev[y], y, w - prev[y], 1);
        prev[y] = w;
      }
    }
    delay(FX_DELAY);
  }
}

void fxDissolve() {                       // random blocks pop in
  const int B = 12, N = 20;
  static uint16_t idx[N * N];
  for (int i = 0; i < N * N; i++) idx[i] = i;
  for (int i = N * N - 1; i > 0; i--) {
    int j = esp_random() % (i + 1);
    uint16_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
  }
  for (int i = 0; i < N * N; i++) {
    restoreRect((idx[i] % N) * B, (idx[i] / N) * B, B, B);
    if (i % 12 == 11) delay(FX_DELAY);
  }
}

void fxChecker() {                        // checkerboard in two waves
  const int B = 30, N = 8;
  for (int phase = 0; phase < 2; phase++) {
    for (int d = 0; d < 2 * N - 1; d++) {
      for (int by = 0; by < N; by++) {
        int bx = d - by;
        if (bx < 0 || bx >= N) continue;
        if (((bx + by) & 1) != phase) continue;
        restoreRect(bx * B, by * B, B, B);
      }
      delay(FX_DELAY);
    }
    delay(60);
  }
}

void fxSlideIn(bool fromBottom) {         // new photo slides in over the old one
  for (int s = 16; s <= 240; s += 16) {
    tft.startWrite();
    if (!fromBottom) {
      tft.setAddrWindow(240 - s, 0, s, 240);
      for (int y = 0; y < 240; y++) tft.writePixels(photoBuf + y * 240, s);
    } else {
      tft.setAddrWindow(0, 240 - s, 240, s);
      for (int y = 0; y < s; y++) tft.writePixels(photoBuf + y * 240, 240);
    }
    tft.endWrite();
    delay(FX_DELAY / 2);
  }
}

void fxHeart() {                          // a heart grows from the centre
  const int C = 6, N = 40;
  static bool done[N][N];
  memset(done, 0, sizeof(done));
  for (float R = 12; R <= 150; R += 9) {
    for (int cy = 0; cy < N; cy++)
      for (int cx = 0; cx < N; cx++) {
        if (done[cy][cx]) continue;
        float x = ((cx * C + C / 2) - 120) / R;
        float y = -((cy * C + C / 2) - 112) / R;
        float a = x * x + y * y - 1;
        if (a * a * a - x * x * y * y * y <= 0) {
          restoreRect(cx * C, cy * C, C, C);
          done[cy][cx] = true;
        }
      }
    delay(FX_DELAY + 4);
  }
}

const int NUM_FX = 13;
void runEffect(int fx) {
  switch (fx) {
    case 0:  fxWipe(0); break;
    case 1:  fxWipe(1); break;
    case 2:  fxWipe(2); break;
    case 3:  fxWipe(3); break;
    case 4:  fxCurtains(); break;
    case 5:  fxBlinds(); break;
    case 6:  fxShape(false); break;
    case 7:  fxShape(true); break;
    case 8:  fxDissolve(); break;
    case 9:  fxChecker(); break;
    case 10: fxSlideIn(false); break;
    case 11: fxSlideIn(true); break;
    case 12: fxHeart(); break;
  }
}

// ---------------- Photos ----------------
int drawBlock(JPEGDRAW *p) {
  for (int row = 0; row < p->iHeight; row++) {
    int yy = p->y + row;
    if (yy >= 240) break;
    int w = p->iWidth;
    if (p->x + w > 240) w = 240 - p->x;
    if (w > 0) memcpy(photoBuf + yy * 240 + p->x, p->pPixels + row * p->iWidth, w * 2);
  }
  return 1;
}

void decodePhoto(int idx) {
  if (jpeg.openFLASH((uint8_t *)PHOTOS[idx].data, PHOTOS[idx].len, drawBlock)) {
    jpeg.decode(0, 0, 0);
    jpeg.close();
  } else {
    Serial.printf("Photo %d: open FAILED\n", idx);
    memset(photoBuf, 0, 240 * 240 * 2);
  }
}

void drawPhotoNow(int idx) {              // no transition (used at boot)
  decodePhoto(idx);
  restoreRect(0, 0, 240, 240);
  drawOverlays();
}

void showPhotoAnimated(int idx) {
  decodePhoto(idx);                       // old photo stays on screen while this happens
  int fx;
  do { fx = esp_random() % NUM_FX; } while (fx == lastFx);
  lastFx = fx;
  runEffect(fx);
  restoreRect(0, 0, 240, 240);            // make sure the whole photo is complete
  drawOverlays();
}

void shuffleOrder(int avoidFirst) {
  for (int i = 0; i < NUM_PHOTOS; i++) order[i] = i;
  for (int i = NUM_PHOTOS - 1; i > 0; i--) {
    int j = esp_random() % (i + 1);
    int t = order[i]; order[i] = order[j]; order[j] = t;
  }
  if (NUM_PHOTOS > 1 && order[0] == avoidFirst) {
    int t = order[0]; order[0] = order[1]; order[1] = t;
  }
}

void pushHistory(int idx) {                  // remember a newly shown photo
  if (histLen == HIST_MAX) {                 // full: forget the oldest
    for (int i = 1; i < HIST_MAX; i++) history[i - 1] = history[i];
    histLen--;
  }
  history[histLen++] = idx;
  histPos = histLen - 1;
}

void nextPhoto() {
  unsigned long t0 = millis();
  int idx;
  if (histPos >= 0 && histPos < histLen - 1) {            // we stepped back earlier: replay forward first
    idx = history[++histPos];
  } else {
    int last = order[pos];
    pos++;
    if (pos >= NUM_PHOTOS) { shuffleOrder(last); pos = 0; }   // a new cycle starts
    idx = order[pos];
    pushHistory(idx);
  }
  greetingUntil = 0;
  showPhotoAnimated(idx);
  lastChange = millis();
  if ((++swapCount & 1) == 0) greetPending = true;        // a greeting after every other swap
  Serial.printf("Photo %d (slot %d of %d), effect %d, %lu ms\n", idx, pos + 1, NUM_PHOTOS, lastFx, millis() - t0);
}

void prevPhoto() {                           // double press: the photo before this one
  if (histPos <= 0) { flashMessage("No earlier photo"); lastChange = millis(); return; }
  unsigned long t0 = millis();
  histPos--;
  greetingUntil = 0;
  showPhotoAnimated(history[histPos]);
  lastChange = millis();
  if ((++swapCount & 1) == 0) greetPending = true;
  Serial.printf("Back to photo %d, effect %d, %lu ms\n", history[histPos], lastFx, millis() - t0);
}

// ---------------- Messages ----------------
void flashMessage(const char *msg) {
  int bx, bw, by, bh;
  measure(msg, &FreeSansBold12pt7b, bx, bw, by, bh);
  int X = (240 - bw) / 2 - bx, Y = 125, R = 1;
  int rx = X + bx - R - 2, ry = Y + by - R - 2, rw = bw + 2 * R + 4, rh = bh + 2 * R + 4;
  GFXcanvas16 *cv = beginPatch(rx, ry, rw, rh);
  if (!cv) return;
  textOutlined(*cv, msg, X - rx, Y - ry, &FreeSansBold12pt7b, ST77XX_WHITE, R);
  endPatch(cv, rx, ry, rw, rh);
  delay(800);
  restoreRect(rx, ry, rw, rh);
}

void showGreeting(const char *msg) {
  const GFXfont *fonts[3] = {&FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b};
  int pick = 2, bx = 0, bw = 0, by = 0, bh = 0;
  for (int i = 0; i < 3; i++) {                           // size from the longest greeting, so all of them match
    measure("Good afternoon", fonts[i], bx, bw, by, bh);
    if (bw <= 216) { pick = i; break; }
  }
  measure(msg, fonts[pick], bx, bw, by, bh);
  int R = (pick == 0) ? 2 : 1;
  int X = (240 - bw) / 2 - bx, Y = GREETING_Y;
  int rx = X + bx - R - 2, ry = Y + by - R - 2, rw = bw + 2 * R + 4, rh = bh + 2 * R + 4;
  GFXcanvas16 *cv = beginPatch(rx, ry, rw, rh);
  if (!cv) return;
  textOutlined(*cv, msg, X - rx, Y - ry, fonts[pick], ST77XX_WHITE, R);
  endPatch(cv, rx, ry, rw, rh);
  gX = rx; gY = ry; gW = rw; gH = rh;
}

// ---------------- Birthday greeting ----------------
// Word-wrap msg into lines no wider than maxW. Returns the line count, or more than
// BDAY_MAX_LINES if it does not fit.
int wrapWords(const char *msg, const GFXfont *f, int maxW, char out[][BDAY_LINE_LEN]) {
  int n = 0;
  out[0][0] = 0;
  const char *p = msg;
  while (*p) {
    while (*p == ' ') p++;                                // also collapses double spaces
    if (!*p) break;
    char word[BDAY_LINE_LEN];
    int w = 0;
    while (*p && *p != ' ' && w < BDAY_LINE_LEN - 1) word[w++] = *p++;
    word[w] = 0;
    char trial[2 * BDAY_LINE_LEN];
    if (out[n][0]) snprintf(trial, sizeof(trial), "%s %s", out[n], word);
    else snprintf(trial, sizeof(trial), "%s", word);
    int bx, bw, by, bh;
    measure(trial, f, bx, bw, by, bh);
    if (bw <= maxW || !out[n][0]) {
      snprintf(out[n], BDAY_LINE_LEN, "%s", trial);
    } else {
      if (++n >= BDAY_MAX_LINES) return BDAY_MAX_LINES + 1;
      snprintf(out[n], BDAY_LINE_LEN, "%s", word);
    }
  }
  return n + 1;                                           // a very long single word stays alone on its line
}

// ---- birthday effects: waving pastel-rainbow letters, hearts and sparkles floating up ----
uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

uint16_t pastel(float hue) {                              // soft rainbow colour, hue in degrees
  float h = fmodf(hue, 360.0f) / 60.0f;
  int i = (int)h;
  float f = h - i, s = 0.55f, v = 1.0f;
  float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  float r, g, b;
  switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return rgb565((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

void drawHeart(Adafruit_GFX &g, int x, int y, int r, uint16_t c) {
  g.fillCircle(x - r, y, r, c);
  g.fillCircle(x + r, y, r, c);
  g.fillTriangle(x - 2 * r, y + 1, x + 2 * r, y + 1, x, y + 3 * r, c);
}

void drawStar(Adafruit_GFX &g, int x, int y, int r, uint16_t c) {   // four-point sparkle, tips r pixels out
  int a = r / 3 + 1;
  int px[8] = {0, a, r, a, 0, -a, -r, -a};
  int py[8] = {-r, -a, 0, a, r, a, 0, -a};
  for (int k = 0; k < 8; k++) {
    int k2 = (k + 1) & 7;
    g.fillTriangle(x, y, x + px[k], y + py[k], x + px[k2], y + py[k2], c);
  }
}

int glyphAdvance(const GFXfont *f, char ch) {             // how far the cursor moves after this character
  uint8_t c = (uint8_t)ch;
  if (c < f->first || c > f->last) return 0;
  return f->glyph[c - f->first].xAdvance;
}

// Like pastel(), with a choice of saturation (the themes of the day greeting).
uint16_t hsv565(float hue, float sat) {
  float h = fmodf(hue, 360.0f) / 60.0f;
  int i = (int)h;
  float f = h - i, v = 1.0f;
  float p = v * (1 - sat), q = v * (1 - sat * f), t = v * (1 - sat * (1 - f));
  float r, g, b;
  switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return rgb565((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

// Letter colour: the birthday message is rainbow, each greeting has the colours of its time of day.
uint16_t themeColor(int theme, int idx, float t) {
  float w = 0.5f + 0.5f * sinf(t * 2.0f + idx * 0.5f);      // colours flow along the word
  switch (theme) {
    case 1: return hsv565(15 + 45 * w, 0.75f);              // morning: orange to yellow
    case 2: return hsv565(175 + 45 * w, 0.55f);             // afternoon: sky blues
    case 3: return hsv565(335 + 50 * w, 0.65f);             // evening: pink to orange
    case 4: return hsv565(215 + 55 * w, 0.40f);             // night: pale blue to violet
    default: return pastel(idx * 28 + t * 140.0f);          // birthday: rainbow
  }
}

uint16_t starColor() {
  switch (msgTheme) {
    case 1: return rgb565(255, 225, 110);
    case 3: return rgb565(255, 190, 170);
    case 4: return rgb565(215, 230, 255);
    default: return rgb565(255, 240, 150);
  }
}

int slideOffset(int idx, float t) {                         // letters fly in from alternate sides, one after another
  float p = (t - 0.05f * idx) / 0.5f;
  if (p >= 1) return 0;
  if (p < 0) p = 0;
  float e = 1 - p;
  return (int)(((idx & 1) ? 160 : -160) * e * e);
}

// One strip of the birthday screen, built off-screen: the clean photo, then the floating hearts and
// sparkles, then (if the strip holds a line of the message) the outlined letters on top.
void drawBirthdayStrip(int ry, int rh, int line, float t) {
  int rx = 0, rw = 240;
  GFXcanvas16 *cv = beginPatch(rx, ry, rw, rh);
  if (!cv) return;
  for (int i = 0; i < N_PARTS; i++) {
    int x = (int)(partX[i] + 4 * sinf(t * 1.5f + partPhase[i]));
    int y = (int)partY[i] - ry;
    if (y < -14 || y > rh + 14) continue;                 // not in this strip
    if ((i & 1) && msgTheme == 0) {                       // heart with a white rim (birthday only)
      int r = (i & 2) ? 4 : 3;
      drawHeart(*cv, x, y, r + 1, ST77XX_WHITE);
      drawHeart(*cv, x, y, r, pastel(330 + (i * 13) % 40));
    } else {                                              // twinkling sparkle with a dark rim
      int r = 2 + (int)(2.5f * (0.5f + 0.5f * sinf(t * 5.0f + partPhase[i])));
      drawStar(*cv, x, y, r + 2, ST77XX_BLACK);
      drawStar(*cv, x, y, r, starColor());
    }
  }
  if (line >= 0) {
    const GFXfont *f = bLineFont[line];
    int R = bLineR[line];
    int len = strlen(bLines[line]);
    int bx, bw, by, bh;
    measure(bLines[line], f, bx, bw, by, bh);
    int cursor = (240 - bw) / 2 - bx;                     // where the first letter starts
    int xs[BDAY_LINE_LEN], dys[BDAY_LINE_LEN];
    for (int k = 0; k < len; k++) {
      xs[k] = cursor + slideOffset(bLineLetters[line] + k, t);
      cursor += glyphAdvance(f, bLines[line][k]);
      dys[k] = (int)lroundf(BDAY_WAVE * sinf(t * 5.0f + (bLineLetters[line] + k) * 0.6f));
    }
    int base = R + BDAY_WAVE + bLineAsc[line];            // baseline inside this strip
    cv->setFont(f);
    cv->setTextSize(1);
    cv->setTextWrap(false);
    cv->setTextColor(ST77XX_BLACK);                       // pass 1: dark outline around every letter
    for (int k = 0; k < len; k++) {
      if (bLines[line][k] == ' ') continue;
      for (int ox = -R; ox <= R; ox++)
        for (int oy = -R; oy <= R; oy++)
          if ((ox || oy) && ox * ox + oy * oy <= R * R + 1) {
            cv->setCursor(xs[k] + ox, base + dys[k] + oy);
            cv->print(bLines[line][k]);
          }
    }
    for (int k = 0; k < len; k++) {                       // pass 2: the coloured letters
      if (bLines[line][k] == ' ') continue;
      cv->setTextColor(themeColor(msgTheme, bLineLetters[line] + k, t));
      cv->setCursor(xs[k], base + dys[k]);
      cv->print(bLines[line][k]);
    }
  }
  endPatch(cv, rx, ry, rw, rh);
}

// One animation frame over the whole screen: strips above the message, one per line, and below it.
void drawBirthdayFrame() {
  unsigned long nowMs = millis();
  float t = (nowMs - bdayStart) / 1000.0f;
  float dt = lastBdayFrame ? (nowMs - lastBdayFrame) / 1000.0f : 0.0f;
  if (dt > 0.3f) dt = 0.3f;                               // after a long pause (e.g. a weather fetch) don't jump
  lastBdayFrame = nowMs;
  for (int i = 0; i < N_PARTS; i++) {                     // float upward, come back in at the bottom
    partY[i] -= partSpeed[i] * dt;
    if (partY[i] < partTop - 10) { partY[i] = partBottom + 10; partX[i] = 10 + random(220); }
  }
  for (int y = 0; msgFull && y < bBlockTop;) {                       // above the message
    int h = bBlockTop - y;
    if (h > 56) h = 56;
    drawBirthdayStrip(y, h, -1, t);
    y += h;
  }
  for (int line = 0; line < bN; line++) drawBirthdayStrip(bLineTop[line], bLineH[line], line, t);
  for (int y = bBlockBottom; msgFull && y < 240;) {                  // below the message
    int h = 240 - y;
    if (h > 56) h = 56;
    drawBirthdayStrip(y, h, -1, t);
    y += h;
  }
}

// Sets the message up (wrap, a font for each line, layout, particles) and draws the first frame. The loop
// animates it until the greeting time is over.
void startMessage(const char *msg, int theme, bool full) {
  const GFXfont *fonts[4] = {&FreeSansBold24pt7b, &FreeSansBold18pt7b, &FreeSansBold12pt7b, &FreeSansBold9pt7b};
  int n = BDAY_MAX_LINES + 1;
  for (int i = full ? 0 : 1; i < (full ? 4 : 2) && n > BDAY_MAX_LINES; i++) n = wrapWords(msg, fonts[i], full ? 232 : 150, bLines);   // greetings: always 18 pt, two lines
  if (n > BDAY_MAX_LINES) n = BDAY_MAX_LINES;             // even the smallest font overflows: show what fits
  bN = n;
  int total = 0, letters = 0;
  for (int i = 0; i < n; i++) {                           // each line: the biggest font that fits the width
    int pick = full ? 3 : 1, bx, bw, by, bh;
    for (int f = full ? 0 : 1; f < (full ? 4 : 2); f++) {
      measure(bLines[i], fonts[f], bx, bw, by, bh);
      if (bw <= 232) { pick = f; break; }
    }
    bLineFont[i] = fonts[pick];
    bLineR[i] = (pick <= 1) ? 2 : 1;                      // thicker outline for the big letters
    measure("Hg", fonts[pick], bx, bw, by, bh);           // by = top of the letters above the baseline
    bLineAsc[i] = -by;
    bLineH[i] = -by + (by + bh) + 2 * (bLineR[i] + BDAY_WAVE);   // a strip tall enough for outline and wave
    bLineLetters[i] = letters;
    letters += strlen(bLines[i]);
    total += bLineH[i];
  }
  bBlockTop = (full ? 120 : 118) - total / 2;                            // the message block, centred on the screen
  int y = bBlockTop;
  for (int i = 0; i < n; i++) { bLineTop[i] = y; y += bLineH[i]; }
  bBlockBottom = y;
  msgFull = full;
  msgTheme = theme;
  partTop = full ? 0 : bBlockTop - 8;
  partBottom = full ? 240 : bBlockBottom + 8;
  for (int i = 0; i < N_PARTS; i++) {
    partX[i] = 10 + random(220);
    partY[i] = partTop + random(partBottom - partTop);
    partSpeed[i] = 14 + random(20);                       // pixels per second
    partPhase[i] = random(628) / 100.0f;
  }
  gX = 0; gW = 240;                                       // the area restored when the message ends
  gY = full ? 0 : bBlockTop;
  gH = full ? 240 : bBlockBottom - bBlockTop;
  bdayStart = millis();
  lastBdayFrame = 0;
  bdayAnimating = true;
  drawBirthdayFrame();
}

void showBirthdayGreeting(const char *msg) { startMessage(msg, 0, true); }
bool screenCovered() { return bdayAnimating && msgFull; }   // the message hides the clock, weather and Wi-Fi icon

const char *birthdayToday() {                             // today's message, or nullptr
  struct tm t;
  if (!getLocalTime(&t, 0)) return nullptr;
  for (const Birthday &b : BIRTHDAYS)
    if (b.month == t.tm_mon + 1 && b.day == t.tm_mday) return b.msg;
  return nullptr;
}

// Shown at power-up and then every BDAY_EVERY_MIN minutes on the clock (:00, :15, :30, :45), on a
// birthday only. Waits until the clock is known.
void checkBirthday() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  int perHour = 60 / BDAY_EVERY_MIN;
  int slot = (t.tm_yday * 24 + t.tm_hour) * perHour + t.tm_min / BDAY_EVERY_MIN;   // which quarter hour it is
  if (!bdayPending && slot == lastBdaySlot) return;       // nothing new since the last check
  bdayPending = false;
  lastBdaySlot = slot;
  const char *msg = birthdayToday();
  if (!msg) return;
  if (greetingUntil) restoreRect(gX, gY, gW, gH);         // replace a regular greeting that is showing
  showBirthdayGreeting(msg);
  greetingUntil = millis() + BDAY_GREETING_MS;
}

// ---------------- Time of day ----------------
int currentPeriod() {                  // 1 morning, 2 afternoon, 3 evening, 4 night, -1 time unknown
  struct tm t;
  if (!getLocalTime(&t, 0)) return -1;
  int h = t.tm_hour;
  if (h >= 5 && h < 12) return 1;
  if (h >= 12 && h < 17) return 2;
  if (h >= 17 && h < 21) return 3;
  return 4;                            // 21:00 to 04:59
}

// A time-of-day greeting after every other photo swap (greetPending is set by the swap).
void checkGreeting() {
  if (!greetPending) return;
  greetPending = false;
  if (greetingUntil) return;                              // a birthday message is showing: it wins
  int p = currentPeriod();
  if (p < 0) return;                                      // the clock is not known yet
  const char *msg = nullptr;
  if (p == 1) msg = "Good morning";
  else if (p == 2) msg = "Good afternoon";
  else if (p == 3) msg = "Good evening";
  else if (p == 4) msg = "Good night";
  if (msg) { startMessage(msg, p, false); greetingUntil = millis() + GREETING_MS; }
}

// ---------------- Clock, restarts and Wi-Fi recovery ----------------
bool timeIsValid() { return time(nullptr) > 1700000000; }   // a real date, not 1970 (the board has no battery clock)

// Save the clock and the weather in RTC memory so a restart does not blank them.
void saveRtcState() {
  time_t t = time(nullptr);
  if (t > 1700000000) rtcTime = t;
  rtcTemp = wTemp; rtcCode = wCode; rtcDay = wDay;
  rtcWeatherAt = weatherOk ? weatherEpoch : 0;
  rtcMagic = RTC_MAGIC;
}

void restoreRtcState() {
  if (rtcMagic != RTC_MAGIC) return;                        // power-up or new firmware: nothing was saved
  if (rtcTime > 1700000000) {
    struct timeval tv = {rtcTime + 3, 0};                   // about 3 s passed during the restart
    settimeofday(&tv, nullptr);
    Serial.println("Clock restored from before the restart");
  }
  if (rtcWeatherAt > 1700000000 && rtcTime - rtcWeatherAt < 6 * 3600) {
    wTemp = rtcTemp; wCode = rtcCode; wDay = rtcDay;
    weatherEpoch = rtcWeatherAt;
    weatherOk = true;
    Serial.println("Weather restored from before the restart");
  }
}

void restartNow(const char *why) {
  Serial.printf("Restarting: %s\n", why);
  saveRtcState();
  strncpy(rtcWhy, why, sizeof(rtcWhy) - 1);
  rtcWhy[sizeof(rtcWhy) - 1] = 0;
  delay(200);
  ESP.restart();
}

void setupWifiRadio() {
  WiFi.persistent(false);                                   // do not rewrite the flash on every connect
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(WIFI_POWER_SAVE);                           // radio always awake: far fewer dropped connections
}

void resetWifiRadio() {                                     // the last step before a restart: radio off and on
  Serial.println("WiFi: resetting the radio");
  WiFi.disconnect(true, false);
  delay(300);
  WiFi.mode(WIFI_OFF);
  delay(300);
  setupWifiRadio();
  wifiMulti.run();
}

// Once a second: notice a lost connection and work through the fixes, quickest first.
void wifiWatchdog(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiDownSince) Serial.printf("WiFi back after %lu s\n", (now - wifiDownSince) / 1000);
    wifiDownSince = 0;
    wifiAttempts = 0;
    radioResetDone = false;
    if (!timeIsValid() && now - lastNtpKick >= 20000) {     // clock still unknown: ask for the time again
      lastNtpKick = now;
      configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
    }
    return;
  }
  if (!wifiDownSince) {                                     // just lost it: quick retry of the last network
    wifiDownSince = now;
    lastWifiTry = now;
    wifiAttempts = 0;
    Serial.println("WiFi lost");
    WiFi.reconnect();
    return;
  }
  unsigned long down = now - wifiDownSince;
  unsigned long gap = wifiAttempts == 0 ? 15000UL : wifiAttempts == 1 ? 30000UL : wifiAttempts == 2 ? 60000UL : 120000UL;
  if (now - lastWifiTry >= gap) {                           // scan for the best known network and join it
    lastWifiTry = now;
    wifiAttempts++;
    Serial.printf("WiFi down %lu s, trying again (%d)\n", down / 1000, wifiAttempts);
    wifiMulti.run();
  }
  if (down >= WIFI_RADIO_RESET_MS && !radioResetDone) {
    radioResetDone = true;
    resetWifiRadio();
  }
  if (down >= WIFI_RESTART_MS) restartNow("WiFi down 6 h");
}

// The last good weather (and the server's address) is also kept in flash, so it survives a power cut.
Preferences prefs;
void saveWeatherFlash() {
  if (!prefs.begin("frame", false)) return;
  prefs.putInt("wt", wTemp);
  prefs.putInt("wc", wCode);
  prefs.putBool("wd", wDay);
  prefs.putULong64("we", (uint64_t)weatherEpoch);
  prefs.putUInt("wip", weatherIpCache);
  prefs.end();
}

void loadWeatherFlash() {
  if (!prefs.begin("frame", true)) return;                  // nothing saved yet
  weatherIpCache = prefs.getUInt("wip", 0);
  if (!weatherOk && prefs.isKey("we")) {
    wTemp = prefs.getInt("wt", 0);
    wCode = prefs.getInt("wc", 0);
    wDay = prefs.getBool("wd", true);
    weatherEpoch = (time_t)prefs.getULong64("we", 0);
    weatherOk = true;
    Serial.println("Weather restored from flash");
  }
  prefs.end();
}

// Hide weather that is too old to trust (when updates have failed for hours). Returns true when it hid it.
bool expireOldWeather() {
  if (weatherOk && weatherEpoch > 1700000000 && timeIsValid() && time(nullptr) - weatherEpoch > WEATHER_STALE_S) {
    weatherOk = false;
    Serial.println("Weather too old: hidden until the next update");
    return true;
  }
  return false;
}

// ---------------- Weather (Open-Meteo, no account needed) ----------------
bool findNumber(const String &s, int from, const char *key, float &out) {
  String k = String("\"") + key + "\":";
  int i = s.indexOf(k, from);
  if (i < 0) return false;
  out = s.substring(i + k.length()).toFloat();
  return true;
}

// One request to the weather server. Returns the HTTP code (200 = good) and fills body.
int weatherGet(WiFiClient &client, const String &url, String &body) {
  HTTPClient http;
  int code = -100;
  if (http.begin(client, url)) {
    http.setConnectTimeout(8000);                      // a weak link can need a few retransmissions
    http.setTimeout(10000);
    http.useHTTP10(true);                              // plain reply, no chunked encoding
    code = http.GET();
    if (code == 200) body = http.getString();
    http.end();
  }
  return code;
}

// Plain HTTP is tried first: it needs far less memory and far fewer packets than HTTPS, which matters on a
// weak Wi-Fi link. It is public weather data, so nothing private is sent either way.
int weatherRequest(bool secure, String &body) {
  String url = String(secure ? "https://" : "http://") + "api.open-meteo.com/v1/forecast?latitude=" +
               String(LATITUDE, 4) + "&longitude=" + String(LONGITUDE, 4) +
               "&current=temperature_2m,weather_code,is_day&timezone=auto";
  if (secure) {
    WiFiClientSecure client;
    client.setInsecure();                              // skip the certificate check (fine for weather data)
    return weatherGet(client, url, body);
  }
  WiFiClient client;
  return weatherGet(client, url, body);
}

// The weather server's address. If DNS fails on a weak link, the last address that worked is used.
bool weatherServerIp(IPAddress &ip) {
  if (WiFi.hostByName("api.open-meteo.com", ip) == 1) {
    weatherIpCache = (uint32_t)ip;                          // saved to flash with the next good weather
    return true;
  }
  if (weatherIpCache) {
    ip = IPAddress(weatherIpCache);
    Serial.println("Weather: DNS failed, using the last known address");
    return true;
  }
  return false;
}

// Plain HTTP straight to the server's address (the Host header still names the real server).
int weatherGetHttp(const String &path, String &body) {
  IPAddress ip;
  if (!weatherServerIp(ip)) return -101;
  WiFiClient client;
  if (!client.connect(ip, 80, 6000)) return -102;           // could not connect within 6 s
  HTTPClient http;
  int code = -100;
  if (http.begin(client, "api.open-meteo.com", 80, path, false)) {
    http.setTimeout(8000);
    http.useHTTP10(true);                                   // plain reply, no chunked encoding
    code = http.GET();
    if (code == 200) body = http.getString();
    http.end();
  }
  client.stop();
  return code;
}

bool fetchWeather() {
  Serial.printf("Weather: fetching (signal %d dBm, free memory %u)\n", (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
  if (WiFi.status() != WL_CONNECTED) { Serial.println("Weather: no WiFi"); return false; }
  String path = String("/v1/forecast?latitude=") + String(LATITUDE, 4) + "&longitude=" + String(LONGITUDE, 4) +
                "&current=temperature_2m,weather_code,is_day&timezone=auto";
  String body;
  int code = weatherGetHttp(path, body);                    // plain HTTP first
  if (code != 200 && code != HTTPC_ERROR_READ_TIMEOUT) {    // could not connect: one more quick try
    delay(1000);
    code = weatherGetHttp(path, body);
  }
  if (code != 200 && code != HTTPC_ERROR_READ_TIMEOUT) {    // still no: try HTTPS as well
    Serial.printf("Weather: HTTP code %d, trying HTTPS\n", code);
    code = weatherRequest(true, body);
  }
  Serial.printf("Weather: HTTP code %d\n", code);
  if (code != 200) return false;

  int c = body.indexOf("\"current\":{");
  float temp, wc, day;
  if (c < 0 || !findNumber(body, c, "temperature_2m", temp) ||
      !findNumber(body, c, "weather_code", wc) || !findNumber(body, c, "is_day", day)) {
    Serial.println("Weather: could not read the reply");
    return false;
  }
  wTemp = (int)lroundf(temp);
  wCode = (int)wc;
  wDay = ((int)day == 1);
  weatherOk = true;
  lastWeatherOk = millis();
  weatherEpoch = timeIsValid() ? time(nullptr) : 0;
  Serial.printf("Weather: %d C, code %d\n", wTemp, wCode);
  return true;
}

// The weather is fetched in its own task: on a weak link a fetch can take 20 s or more, and the buttons and
// the game must keep working meanwhile. The task only touches the weather variables, never the display.
volatile int weatherTaskState = 0;                          // 0 idle, 1 running, 2 finished OK, 3 finished with an error
void weatherTask(void *) {
  weatherTaskState = fetchWeather() ? 2 : 3;
  vTaskDelete(nullptr);
}
void startWeatherTask() {
  weatherTaskState = 1;
  if (xTaskCreate(weatherTask, "weather", 12288, nullptr, 1, nullptr) != pdPASS) weatherTaskState = 3;
}

// ---------------- Button (debounced) ----------------
void handleButton() {
  unsigned long now = millis();
  bool raw = (digitalRead(BTN_PIN) == LOW);
  if (raw != btnRaw) { btnRaw = raw; btnChangedAt = now; }

  if (btnStable != btnRaw && now - btnChangedAt >= DEBOUNCE_MS) {   // only accept a settled state
    btnStable = btnRaw;
    if (btnStable) downAt = now;                      // pressed
    else { clickCount++; lastReleaseAt = now; }       // released: one more press in the series
  }
  if (clickCount && !btnStable && now - lastReleaseAt > DOUBLE_PRESS_MS) {   // the series of presses is over
    int n = clickCount;
    clickCount = 0;
    if (n == 1) nextPhoto();                          // one press: the next photo
    else if (n == 2) prevPhoto();                     // two: the photo before
    else runGame();                                   // three: the game
  }

  // Hold for 5 seconds: restart. Fresh clock reading: the game or a photo change above may have taken a
  // long time, and a stale 'now' older than downAt would wrap around and restart the frame by mistake.
  now = millis();
  if (btnStable && (long)(now - downAt) >= (long)RESET_HOLD_MS) {
    flashMessage("Restarting");
    restartNow("button held 5 s");
  }
}

// ---------------- Display self-check ----------------
// The ST7789 keeps its settings in registers. A glitch on the SPI lines can flip one (colours inverted,
// picture upside down) and the panel then keeps it until it is told again. The display cannot be read
// back, so the settings are simply re-sent every few seconds and a glitch corrects itself.
void refreshDisplayConfig() {
  const uint8_t colmod = 0x55;               // 16-bit colour
  tft.sendCommand(ST77XX_COLMOD, &colmod, 1);
  tft.setRotation(DISPLAY_ROTATION);         // memory access direction, as set at start-up
  tft.invertDisplay(true);                   // this panel needs inversion ON for correct colours
  tft.sendCommand(ST77XX_NORON);
  tft.sendCommand(ST77XX_DISPON);
}

// ---------------- Loading screen ----------------
// Shown from power-up until the clock, date and weather are ready (or LOADING_TIMEOUT_MS passes). It runs
// in its own task, so the stickers keep bouncing while setup() waits for the Wi-Fi, the clock and the
// weather. The task owns the display until stopLoader() returns, so setup() must not draw before that.
volatile bool loaderStop = false, loaderRunning = false;
const int LOADER_STRIP_H = 48;                            // the screen is drawn in 5 strips of 48 rows

struct Sticker { float x, y, vx, vy; };

uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {  // a = how much of the sprite colour (0..255)
  uint32_t r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (255 - a)) / 255;
  uint32_t g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a)) / 255;
  uint32_t b = ((fg & 31) * a + (bg & 31) * (255 - a)) / 255;
  return (r << 11) | (g << 5) | b;
}

void loaderRun() {
  GFXcanvas16 cv(240, LOADER_STRIP_H);
  uint16_t *buf = cv.getBuffer();
  if (!buf) return;                                       // no memory: no loading screen, setup() just carries on
  const char *text = "Loading...";
  const int len = 10;                                     // 7 letters and 3 dots
  const GFXfont *font = &FreeSansBold24pt7b;
  cv.setFont(font);
  cv.setTextSize(1);
  cv.setTextWrap(false);
  int16_t tx, ty;
  uint16_t tw, th;
  cv.getTextBounds(text, 0, 0, &tx, &ty, &tw, &th);
  int xs[len];
  int cursor = (240 - (int)tw) / 2 - tx;
  for (int k = 0; k < len; k++) { xs[k] = cursor; cursor += glyphAdvance(font, text[k]); }
  const int baseline = 136;                               // capitals centred around y = 120

  Sticker s[N_STICKERS];                                  // four stickers, starting apart and moving off in random directions
  for (int i = 0; i < N_STICKERS; i++) {
    s[i].x = 10 + (i % 2) * 130 + random(30);
    s[i].y = 10 + (i / 2) * 130 + random(30);
    float speed = 45 + random(30);                        // pixels per second
    float ang = random(360) * 0.0174533f;
    s[i].vx = speed * cosf(ang);
    s[i].vy = speed * sinf(ang);
    if (fabsf(s[i].vx) < 20) s[i].vx = (s[i].vx < 0) ? -20 : 20;   // never crawl along one edge
    if (fabsf(s[i].vy) < 20) s[i].vy = (s[i].vy < 0) ? -20 : 20;
  }
  int starX[28], starY[28];
  float starPh[28];
  for (int i = 0; i < 28; i++) { starX[i] = 2 + random(236); starY[i] = 2 + random(236); starPh[i] = random(628) / 100.0f; }

  unsigned long t0 = millis(), last = t0;
  while (!loaderStop) {
    unsigned long frameStart = millis();
    float t = (frameStart - t0) / 1000.0f;
    float dt = (frameStart - last) / 1000.0f;
    last = frameStart;
    if (dt > 0.2f) dt = 0.2f;
    for (int i = 0; i < N_STICKERS; i++) {                // move, and bounce off the edges of the screen
      int w = STICKER_W[i], h = STICKER_H[i];
      s[i].x += s[i].vx * dt;
      s[i].y += s[i].vy * dt;
      if (s[i].x < 0) { s[i].x = 0; s[i].vx = fabsf(s[i].vx); }
      if (s[i].x > 240 - w) { s[i].x = 240 - w; s[i].vx = -fabsf(s[i].vx); }
      if (s[i].y < 0) { s[i].y = 0; s[i].vy = fabsf(s[i].vy); }
      if (s[i].y > 240 - h) { s[i].y = 240 - h; s[i].vy = -fabsf(s[i].vy); }
    }
    int dy[len];                                          // the letters wave, the three dots hop one after another
    for (int k = 0; k < len; k++) {
      if (k < 7) dy[k] = (int)lroundf(4 * sinf(t * 4.0f + k * 0.7f));
      else dy[k] = -(int)(10 * fabsf(sinf(t * 5.0f - (k - 7) * 0.9f)));
    }
    for (int y0 = 0; y0 < 240; y0 += LOADER_STRIP_H) {
      for (int r = 0; r < LOADER_STRIP_H; r++) {          // background: a dark purple gradient
        int y = y0 + r;
        cv.drawFastHLine(0, r, 240, rgb565(18 + 54 * y / 240, 10 + 18 * y / 240, 48 + 62 * y / 240));
      }
      for (int i = 0; i < 28; i++) {                      // twinkling stars
        int y = starY[i] - y0;
        if (y < 0 || y >= LOADER_STRIP_H) continue;
        uint8_t v = 90 + (int)(150 * (0.5f + 0.5f * sinf(t * 3.0f + starPh[i])));
        cv.drawPixel(starX[i], y, rgb565(v, v, v));
        if (v > 200) cv.fillRect(starX[i] - 1, y, 3, 1, rgb565(v, v, v));
      }
      for (int i = 0; i < N_STICKERS; i++) {              // stickers, blended over the background
        int w = STICKER_W[i], h = STICKER_H[i];
        int sx = (int)s[i].x, sy = (int)s[i].y;
        int r0 = max(0, y0 - sy), r1 = min(h, y0 + LOADER_STRIP_H - sy);   // sprite rows inside this strip
        const uint16_t *rgb = STICKER_RGB[i];
        const uint8_t *al = STICKER_A[i];
        for (int r = r0; r < r1; r++) {
          uint16_t *dst = buf + (sy + r - y0) * 240 + sx;
          const uint16_t *src = rgb + r * w;
          const uint8_t *sa = al + r * w;
          for (int c = 0; c < w; c++) {
            uint8_t a = sa[c];
            if (!a) continue;
            dst[c] = (a == 255) ? src[c] : blend565(src[c], dst[c], a);
          }
        }
      }
      if (y0 + LOADER_STRIP_H > baseline - 50 && y0 < baseline + 22) {   // the text, on top
        cv.setFont(font);
        cv.setTextColor(ST77XX_BLACK);                    // pass 1: dark outline around every letter
        for (int k = 0; k < len; k++)
          for (int ox = -2; ox <= 2; ox++)
            for (int oy = -2; oy <= 2; oy++)
              if ((ox || oy) && ox * ox + oy * oy <= 5) {
                cv.setCursor(xs[k] + ox, baseline + dy[k] + oy - y0);
                cv.print(text[k]);
              }
        for (int k = 0; k < len; k++) {                   // pass 2: rainbow colours that slide along the word
          cv.setTextColor(pastel(k * 32 + t * 160.0f));
          cv.setCursor(xs[k], baseline + dy[k] - y0);
          cv.print(text[k]);
        }
      }
      if (bootReason[0] && y0 + LOADER_STRIP_H >= 240) {  // after a restart: say why, in small print at the bottom
        cv.setFont(nullptr);
        cv.setTextSize(1);
        cv.setTextColor(rgb565(200, 200, 220));
        cv.setCursor(4, 228 - y0);
        cv.print(bootReason);
      }
      blit(0, y0, 240, LOADER_STRIP_H, buf);
    }
    unsigned long spent = millis() - frameStart;
    delay(spent < 40 ? 40 - spent : 1);                  // about 20 frames a second, and let other tasks run
  }
}

void loaderTask(void *) {
  loaderRun();                                            // the canvas is freed when this returns
  loaderRunning = false;
  vTaskDelete(nullptr);
}

void startLoader() {
  loaderStop = false;
  loaderRunning = true;
  if (xTaskCreatePinnedToCore(loaderTask, "loader", 8192, nullptr, 1, nullptr, 1) != pdPASS) loaderRunning = false;
}

void stopLoader() {                                       // wait until the task has let go of the display
  loaderStop = true;
  unsigned long t0 = millis();
  while (loaderRunning && millis() - t0 < 3000) delay(10);
}

// ---------------- Game: a sticker flaps through pipes (press the button three times) ----------------
const int GAME_STRIP_H = 48;
const int BIRD_X = 56, PIPE_W = 34, GAP_H = 92, CAP_H = 12, PIPE_SPACING = 150;
const float GRAVITY = 900.0f, JUMP_V = -290.0f, PIPE_SPEED = 90.0f;
struct Pipe { float x; int gapY; bool scored; };
volatile bool gameTap = false;
volatile unsigned long lastTapUs = 0;
int gameBest = 0;

// The high score lives on the SD card (file /highscore.txt), so it survives a power cut. The card is only
// mounted for the moment it is read or written, so a missing card or a pulled card never hurts anything.
// It shares the SPI bus with the display, so this must never run while the loading screen task is drawing.
int readHighScoreSd() {
  int v = -1;                                             // -1 = no card or no file
  if (SD.begin(SD_CS, SPI, 10000000)) {
    File f = SD.open("/highscore.txt", FILE_READ);
    if (f) { v = f.parseInt(); f.close(); }
    SD.end();
  } else Serial.println("SD: no card");
  return (v < 0 || v > 99999) ? -1 : v;
}

bool writeHighScoreSd(int v) {
  bool ok = false;
  if (SD.begin(SD_CS, SPI, 10000000)) {
    File f = SD.open("/highscore.txt", FILE_WRITE);       // FILE_WRITE starts the file afresh
    if (f) { f.printf("%d\n", v); f.close(); ok = true; }
    SD.end();
  }
  Serial.printf("SD: high score %d %s\n", v, ok ? "saved" : "NOT saved");
  return ok;
}

void IRAM_ATTR tapIsr() {                                 // every button press, even a very short one
  unsigned long n = micros();
  if (n - lastTapUs > 60000) { gameTap = true; lastTapUs = n; }
}

void fillBuf(uint16_t *buf, int y0, int x, int y, int w, int h, uint16_t c) {   // a rectangle, clipped to this strip
  int ya = max(y, y0), yb = min(y + h, y0 + GAME_STRIP_H), xa = max(x, 0), xb = min(x + w, 240);
  for (int yy = ya; yy < yb; yy++)
    for (int xx = xa; xx < xb; xx++) buf[(yy - y0) * 240 + xx] = c;
}

void pipePiece(uint16_t *buf, int y0, int x, int y, int w, int h) {   // green pipe piece: dark edge, light stripe
  fillBuf(buf, y0, x, y, w, h, 0x0000);
  fillBuf(buf, y0, x + 2, y + 2, w - 4, h - 4, rgb565(70, 200, 95));
  fillBuf(buf, y0, x + 5, y + 2, 4, h - 4, rgb565(150, 238, 160));
}

// The photo stays on screen behind the game. Each press makes the sticker jump; touching a pipe, the top or
// the bottom ends the game with "Bomboclat". A press then plays again; after 7 s without one, back to the photos.
void runGame() {
  if (ESP.getFreeHeap() < 40000) { Serial.println("Game: not enough memory"); return; }
  GFXcanvas16 cv(240, GAME_STRIP_H);
  uint16_t *buf = cv.getBuffer();
  if (!buf) return;                                       // no memory: no game
  greetingUntil = 0;
  bdayAnimating = false;
  const int skin = random(N_STICKERS);                    // one of the four loading-screen stickers is the player
  const int sw = STICKER_S_W[skin], sh = STICKER_S_H[skin];
  const uint16_t *srgb = STICKER_S_RGB[skin];
  const uint8_t *sal = STICKER_S_A[skin];
  const GFXfont *f24 = &FreeSansBold24pt7b, *f18 = &FreeSansBold18pt7b, *f12 = &FreeSansBold12pt7b;
  const char *over = "Bomboclat";
  const int overLen = 9;
  Pipe pipes[4];
  int nPipes = 0, score = 0, state = 0;                   // state: 0 ready, 1 playing, 2 game over
  float birdY = 110, birdV = 0;
  unsigned long t0 = millis(), last = t0, overAt = 0, heldSince = 0, readyAt = t0;
  gameTap = false;
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), tapIsr, FALLING);
  while (true) {
    unsigned long now = millis();
    float dt = (now - last) / 1000.0f;
    if (dt > 0.05f) dt = 0.05f;
    last = now;
    float t = (now - t0) / 1000.0f;
    if (digitalRead(BTN_PIN) == LOW) {                    // holding the button for 5 s still restarts the frame
      if (!heldSince) heldSince = now;
      else if (now - heldSince > RESET_HOLD_MS) restartNow("button held 5 s");
    } else heldSince = 0;
    bool tap = gameTap;
    gameTap = false;

    if (state == 0) {                                     // ready: the sticker hovers until the first press
      birdY = 110 + 6 * sinf(t * 4.0f);
      if (tap) { state = 1; birdV = JUMP_V; }
      else if (now - readyAt > GAME_IDLE_MS) break;       // nobody is playing: never wait forever
    } else if (state == 1) {
      if (tap) birdV = JUMP_V;
      birdV += GRAVITY * dt;
      birdY += birdV * dt;
      for (int i = 0; i < nPipes; i++) pipes[i].x -= PIPE_SPEED * dt;
      if (nPipes && pipes[0].x < -PIPE_W - 10) { for (int i = 1; i < nPipes; i++) pipes[i - 1] = pipes[i]; nPipes--; }
      if ((nPipes == 0 || pipes[nPipes - 1].x < 240 - PIPE_SPACING) && nPipes < 4) {
        pipes[nPipes].x = 260.0f;
        pipes[nPipes].gapY = 30 + (int)random(89);        // top of the gap, 30 to 118
        pipes[nPipes].scored = false;
        nPipes++;
      }
      int hx = BIRD_X + 5, hy = (int)birdY + 5, hw = sw - 10, hh = sh - 10;   // a slightly smaller hit box
      bool hit = hy < 0 || hy + hh > 240;
      for (int i = 0; i < nPipes; i++) {
        int px = (int)pipes[i].x - 3, pw = PIPE_W + 6;
        if (hx + hw > px && hx < px + pw && (hy < pipes[i].gapY || hy + hh > pipes[i].gapY + GAP_H)) hit = true;
        if (!pipes[i].scored && pipes[i].x + PIPE_W < BIRD_X) { pipes[i].scored = true; score++; }
      }
      if (hit) { state = 2; overAt = now; birdV = -120.0f; if (score > gameBest) { gameBest = score; writeHighScoreSd(gameBest); } }
    } else {                                              // game over: the sticker drops to the bottom
      birdV += GRAVITY * dt;
      birdY += birdV * dt;
      if (birdY > 240 - sh) { birdY = 240 - sh; birdV = 0; }
      if (birdY < -40) birdY = -40;                       // keeps the sprite drawing inside the buffer
      if (tap && now - overAt > 700) { nPipes = 0; score = 0; birdY = 110; birdV = 0; state = 0; readyAt = now; }   // play again
      else if (now - overAt > 7000) break;                // back to the photos
    }

    float to = (now - overAt) / 1000.0f;                  // seconds since the game ended
    for (int y0 = 0; y0 < 240; y0 += GAME_STRIP_H) {
      memcpy(buf, photoBuf + y0 * 240, 240 * GAME_STRIP_H * 2);              // the photo behind everything
      if (state == 2) for (int i = 0; i < 240 * GAME_STRIP_H; i++) buf[i] = (buf[i] >> 1) & 0x7BEF;   // dimmed after the crash
      for (int i = 0; i < nPipes; i++) {
        int px = (int)pipes[i].x, gy = pipes[i].gapY;
        pipePiece(buf, y0, px, -6, PIPE_W, gy - CAP_H + 6);                  // top pipe and its cap
        pipePiece(buf, y0, px - 3, gy - CAP_H, PIPE_W + 6, CAP_H);
        pipePiece(buf, y0, px - 3, gy + GAP_H, PIPE_W + 6, CAP_H);           // bottom cap and pipe
        pipePiece(buf, y0, px, gy + GAP_H + CAP_H, PIPE_W, 240 - (gy + GAP_H + CAP_H) + 6);
      }
      int by0 = (int)birdY;                               // the sticker
      int r0 = max(0, y0 - by0), r1 = min(sh, y0 + GAME_STRIP_H - by0);
      for (int r = r0; r < r1; r++) {
        uint16_t *dst = buf + (by0 + r - y0) * 240 + BIRD_X;
        for (int c = 0; c < sw; c++) {
          uint8_t a = pgm_read_byte(&sal[r * sw + c]);
          if (!a) continue;
          uint16_t px = srgb[r * sw + c];
          dst[c] = (a == 255) ? px : blend565(px, dst[c], a);
        }
      }
      int tx, tw, ty, th;
      char line[40];
      if (state == 1) {                                   // the score at the top
        snprintf(line, sizeof(line), "%d", score);
        measure(line, f24, tx, tw, ty, th);
        textOutlinedC(cv, line, (240 - tw) / 2 - tx, 40 - y0, f24, ST77XX_WHITE, ST77XX_BLACK, 2);
      } else if (state == 0) {
        measure("Tap to flap!", f18, tx, tw, ty, th);
        textOutlinedC(cv, "Tap to flap!", (240 - tw) / 2 - tx, 190 - y0, f18, ST77XX_WHITE, ST77XX_BLACK, 2);
      } else {
        if (y0 + GAME_STRIP_H > 10 && y0 < 160) {         // "Bomboclat": letters drop in, bounce, and shake
          measure(over, f24, tx, tw, ty, th);
          int cursor = (240 - tw) / 2 - tx;
          int shake = to < 0.8f ? (int)(4 * sinf(to * 60.0f) * (1 - to / 0.8f)) : 0;
          int xs[overLen], ys[overLen];
          for (int k = 0; k < overLen; k++) {
            float p = (to - 0.06f * k) / 0.45f;
            if (p < 0) p = 0;
            if (p > 1) p = 1;
            xs[k] = cursor + shake;
            ys[k] = 118 - (int)(90.0f * (1 - p) * (1 - p) * cosf(p * 7.0f));
            cursor += glyphAdvance(f24, over[k]);
          }
          cv.setFont(f24);
          cv.setTextSize(1);
          cv.setTextWrap(false);
          cv.setTextColor(ST77XX_BLACK);                  // dark outline, then red-to-yellow letters
          for (int k = 0; k < overLen; k++)
            for (int ox = -2; ox <= 2; ox++)
              for (int oy = -2; oy <= 2; oy++)
                if ((ox || oy) && ox * ox + oy * oy <= 5) {
                  cv.setCursor(xs[k] + ox, ys[k] + oy - y0);
                  cv.print(over[k]);
                }
          for (int k = 0; k < overLen; k++) {
            cv.setTextColor(hsv565(350 + 50 * (0.5f + 0.5f * sinf(t * 6.0f + k * 0.6f)), 0.85f));
            cv.setCursor(xs[k], ys[k] - y0);
            cv.print(over[k]);
          }
        }
        if (to > 0.5f) {
          snprintf(line, sizeof(line), "Score %d   Best %d", score, gameBest);
          const GFXfont *sf = f18;
          measure(line, sf, tx, tw, ty, th);
          if (tw > 232) { sf = f12; measure(line, sf, tx, tw, ty, th); }
          textOutlinedC(cv, line, (240 - tw) / 2 - tx, 172 - y0, sf, ST77XX_WHITE, ST77XX_BLACK, 2);
        }
        if (to > 1.0f && ((int)(to * 2) & 1) == 0) {
          measure("Tap to retry", f12, tx, tw, ty, th);
          textOutlinedC(cv, "Tap to retry", (240 - tw) / 2 - tx, 210 - y0, f12, ST77XX_WHITE, ST77XX_BLACK, 1);
        }
      }
      blit(0, y0, 240, GAME_STRIP_H, buf);
    }
    unsigned long spent = millis() - now;
    delay(spent < 30 ? 30 - spent : 1);                   // about 25 frames a second at best
  }
  detachInterrupt(digitalPinToInterrupt(BTN_PIN));
  btnRaw = btnStable = (digitalRead(BTN_PIN) == LOW);     // the button state as it is now, no phantom presses
  clickCount = 0;
  downAt = millis();
  restoreRect(0, 0, 240, 240);                            // back to the photo, with the clock, weather and Wi-Fi icon
  drawOverlays();
  lastChange = millis();
  lastCornerSwap = millis();
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);
  unsigned long bootStart = millis();

  // Chip selects high first so neither SPI device listens while the other is used
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);               // backlight on; use analogWrite(BL_PIN, 0-255) to dim
  pinMode(BTN_PIN, INPUT_PULLUP);

  // Why did we start? (power-up, a crash, a watchdog, or our own restart with its reason)
  esp_reset_reason_t rr = esp_reset_reason();
  const bool rtcOk = (rtcMagic == RTC_MAGIC);
  if (rr == ESP_RST_POWERON || !rtcOk) { rtcRestarts = 0; rtcWhy[0] = 0; }
  else rtcRestarts++;
  rtcWhy[sizeof(rtcWhy) - 1] = 0;
  const char *rs = "";
  switch (rr) {
    case ESP_RST_POWERON: rs = ""; break;
    case ESP_RST_SW: rs = rtcWhy[0] ? rtcWhy : "software"; break;
    case ESP_RST_PANIC: rs = "crash"; break;
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: rs = "watchdog"; break;
    case ESP_RST_BROWNOUT: rs = "brownout (power dip)"; break;
    default: rs = "other"; break;
  }
  if (rs[0]) snprintf(bootReason, sizeof(bootReason), "restart %lu: %s", (unsigned long)rtcRestarts, rs);
  rtcWhy[0] = 0;
  Serial.printf("Reset reason code %d (%s)\n", (int)rr, bootReason[0] ? bootReason : "power-up");

  tft.init(240, 240);
  tft.setRotation(DISPLAY_ROTATION);
  tft.fillScreen(ST77XX_BLACK);
  gameBest = max(0, readHighScoreSd());           // before the loading screen starts: the SD card shares the display's SPI bus
  Serial.printf("High score from SD: %d\n", gameBest);
  startLoader();                                // the loading screen takes over the display from here
  delay(1500);                                    // (gives the USB serial monitor time to connect)
  Serial.println("Boot");

  photoBuf = (uint16_t *)(psramFound() ? ps_malloc(240 * 240 * 2) : malloc(240 * 240 * 2));
  if (!photoBuf) {
    stopLoader();
    Serial.println("Photo buffer FAILED (out of memory)");
    tft.fillScreen(ST77XX_RED);
    while (true) delay(1000);
  }

  shuffleOrder(-1);
  pos = 0;
  decodePhoto(order[pos]);                        // ready in memory; it is shown when loading ends
  pushHistory(order[pos]);
  Serial.println("First photo decoded");

  restoreRtcState();
  loadWeatherFlash();                             // after a power cut: the last weather is shown at once                              // after a restart: keep the clock and the weather
  configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
  setupWifiRadio();
  wifiMulti.addAP(WIFI_1_SSID, WIFI_1_PASS);
  if (strlen(WIFI_2_SSID) > 0) wifiMulti.addAP(WIFI_2_SSID, WIFI_2_PASS);

  // Wait for the Wi-Fi, the clock and the weather, but never longer than LOADING_TIMEOUT_MS in total.
  unsigned long deadline = bootStart + LOADING_TIMEOUT_MS;
  if (timeIsValid() && weatherOk) {               // after a restart both were kept: nothing to wait for
    wifiMulti.run();
  } else {
    Serial.println("Connecting to WiFi...");
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (long)(deadline - millis()) > 0 && millis() - t0 < 15000) {
      wifiMulti.run();
      delay(250);
    }
    Serial.printf("WiFi status: %d (3 = connected)\n", (int)WiFi.status());
    if (WiFi.status() == WL_CONNECTED) {
      unsigned long t1 = millis();
      while (!timeIsValid() && (long)(deadline - millis()) > 0 && millis() - t1 < 8000) delay(100);
      Serial.printf("Time sync: %d\n", (int)timeIsValid());
      if (!weatherOk && (long)(deadline - millis()) > 12000) {
        bool ok = fetchWeather();
        weatherFails = ok ? 0 : 1;
        weatherNextTry = millis() + (ok ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS[0]);
        if (ok) saveWeatherFlash();
      }
    }
  }
  if (weatherOk && weatherNextTry == 0) weatherNextTry = millis() + 20000;   // kept from before a restart: refresh soon
  // (anything still missing is picked up by the loop as soon as the Wi-Fi allows)

  while (millis() - bootStart < LOADING_MIN_MS) delay(50);   // the loading screen never just flashes
  stopLoader();
  Serial.printf("Loading screen done after %lu ms (clock %s, weather %s)\n", millis() - bootStart,
                timeIsValid() ? "ok" : "missing", weatherOk ? "ok" : "missing");
  runEffect((int)(esp_random() % NUM_FX));        // reveal the first photo with a transition
  restoreRect(0, 0, 240, 240);
  wifiLevel = computeWifiLevel();                 // so the first overlay already shows the real Wi-Fi state
  drawOverlays();
  lastChange = millis();
  lastCornerSwap = millis();                      // start on the time, then switch every CORNER_SWAP_MS
  Serial.println("Setup done");
  lastWeatherOk = millis();
}

void loop() {
  static unsigned long lastTick = 0;

  handleButton();
  unsigned long now = millis();

  // --- once a second ---
  if (now - lastTick >= 1000) {
    lastTick = now;

    checkBirthday();                              // before the regular greeting so a birthday wins
    checkGreeting();
    if (greetingUntil && now >= greetingUntil) {
      greetingUntil = 0;
      restoreRect(gX, gY, gW, gH);               // erase greeting, photo shows through
      if (gW >= 240 && gH >= 240) drawOverlays();   // the birthday message covered everything: repaint the clock etc.
    }

    struct tm t;
    if (now - lastCornerSwap >= CORNER_SWAP_MS) {   // top-left alternates time / date
      lastCornerSwap = now;
      showDate = !showDate;
      if (!screenCovered()) drawTime();
    } else if (!screenCovered() && !showDate && getLocalTime(&t, 0) && t.tm_min != lastMinute) {
      drawTime();                                   // the minute changed while the time is showing
    }

    if (!screenCovered()) updateWifiIcon();           // the birthday message covers the whole screen

    if (now - lastDisplayRefresh >= DISPLAY_REFRESH_MS) {   // undo any display glitch (see refreshDisplayConfig)
      lastDisplayRefresh = now;
      refreshDisplayConfig();
    }

    wifiWatchdog(now);                            // notices a lost connection and works to get it back

    // Weather: refresh every WEATHER_REFRESH_MS; after a failure retry soon, with growing gaps. The last
    // good weather stays on screen the whole time.
    if (weatherTaskState == 0 && WiFi.status() == WL_CONNECTED && (long)(now - weatherNextTry) >= 0) {
      startWeatherTask();                         // runs in the background; the result is handled below
    } else if (weatherTaskState >= 2) {
      bool ok = (weatherTaskState == 2);
      weatherTaskState = 0;
      if (ok) {
        weatherFails = 0;
        weatherNextTry = millis() + WEATHER_REFRESH_MS;
        saveRtcState();
        saveWeatherFlash();
      } else {
        const int steps = sizeof(WEATHER_RETRY_MS) / sizeof(WEATHER_RETRY_MS[0]);
        unsigned long wait = WEATHER_RETRY_MS[weatherFails < steps ? weatherFails : steps - 1];
        weatherFails++;
        weatherNextTry = millis() + wait;
        Serial.printf("Weather failed (%d in a row), next try in %lu s\n", weatherFails, wait / 1000);
        if (weatherFails == 3) resetWifiRadio();  // "connected" but nothing gets through: reconnect
      }
      if (!screenCovered()) drawWeather();
    }
    // (no restart because of missing weather: old weather is simply hidden, and the retries go on)
    if (expireOldWeather() && !screenCovered()) drawWeather();   // too old to trust: take it off the screen

    if (now - lastRtcSave >= 60000) {             // keep the clock and weather safe for a restart or crash
      lastRtcSave = now;
      saveRtcState();
    }
  }

  // --- birthday message effects ---
  if (bdayAnimating) {
    if (!greetingUntil) bdayAnimating = false;            // the message ended (timer, or a new photo replaced it)
    else if (now - lastBdayFrame >= BDAY_FRAME_MS) drawBirthdayFrame();
  }

  // --- auto advance ---
  if (!greetingUntil && !btnStable && (long)(now - lastChange) >= (long)PHOTO_MS) nextPhoto();   // signed: lastChange may be newer than 'now'

  delay(5);
}