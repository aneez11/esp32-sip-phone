// =====================================================================
//  MAX7219 8x8 x4 chain — SIP speaker status-display test (ESP32-S3)
// =====================================================================
//
// This is not a generic bring-up demo: it plays the *same sequence of screens*
// the SIP speaker shows, so the panel is validated in its real usage context.
//
//   [0] hardware self-test  - MAX7219 built-in display test (all LEDs)
//   [1] boot                - "ESP32 SIP VOICE" scroll
//   [2] setup / AP mode     - "AP SETUP 192.168.4.1" scroll
//   [3] Wi-Fi connecting    - blinking "WIFI"
//   [4] SIP registering     - "REG" + progress spinner
//   [5] ready               - extension + heartbeat pixel
//   [6] incoming call       - blinking "RING" + caller scroll
//   [7] in call             - "TALK" + audio level meter
//   [8] volume              - "VOL 80%" + bar
//   [9] offline             - "OFFLINE" scroll + blinking marker
//
// Each phase prints the firmware log line it corresponds to, so the display
// behaviour can be matched against the real device state machine.
//
// Pins (free on the ESP32-S3 SIP build):
//     DIN  = GPIO 10     VCC = 5V
//     CLK  = GPIO 11     GND = GND
//     CS   = GPIO 12
//
// Wiring rules for a chain of N modules:
//     ESP DIN -> module1 DIN
//     module1 DOUT -> module2 DIN -> module3 DIN -> module4 DIN
//     CLK and CS go to ALL modules in parallel.
//
// CHAIN_REVERSED: on this build the module wired to the ESP32 is the RIGHTMOST
// panel, so the chain runs right-to-left. With CHAIN_REVERSED=1, x=0 maps to
// the leftmost panel and all text reads left-to-right on screen. Set it to 0
// if your chain runs left-to-right (ESP32 on the left).
//
// If the panel stays dark or flickers, the usual causes are:
//   * DIN/CLK swapped, or DIN wired to a DOUT pin
//   * modules not sharing a common ground with the ESP32
//   * 5V supply too weak / not 5V (MAX7219 needs ~5V; 3.3V is marginal)
//   * one module in the chain is faulty (try a single module first)
// =====================================================================

#include <Arduino.h>
#include <ctype.h>
#include <string.h>

// ---- panel / wiring -------------------------------------------------------
static const int PIN_DIN = 10;
static const int PIN_CLK = 11;
static const int PIN_CS  = 12;

static const int MODULES = 4;
static const int WIDTH   = MODULES * 8;

// 1 = the module nearest the ESP32 is the rightmost panel (chain right-to-left).
#define CHAIN_REVERSED 1

// ---- values shown on the panel (mirror the device's web Settings) ----------
#define DEMO_SIP_USER    "2001"
#define DEMO_SIP_SERVER  "10.20.30.20"
#define DEMO_VOLUME_PCT  80
#define DEMO_CALLER      "2001"

// Bit-bang SPI at a deliberately slow, reliable rate (MAX7219 tolerates up to
// 10 MHz, but slow rules out signal-integrity issues during bring-up).
#define CLK_DELAY_US 5

#define REG_NOOP        0x00
#define REG_DIGIT0      0x01
#define REG_DECODE      0x09
#define REG_INTENSITY   0x0A
#define REG_SCANLIMIT   0x0B
#define REG_SHUTDOWN    0x0C
#define REG_DISPLAYTEST 0x0F

// ---- low level ------------------------------------------------------------
static void shiftByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_DIN, (b >> i) & 1);
    delayMicroseconds(CLK_DELAY_US);
    digitalWrite(PIN_CLK, HIGH);
    delayMicroseconds(CLK_DELAY_US);
    digitalWrite(PIN_CLK, LOW);
    delayMicroseconds(CLK_DELAY_US);
  }
}

// Send one 16-bit frame; modules are daisy-chained so the last frame shifted
// out ends up in the first module. Put the target data last.
static void maxWrite(uint8_t mod, uint8_t reg, uint8_t data) {
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(CLK_DELAY_US);
  for (int i = MODULES - 1; i >= 0; i--) {
    shiftByte((i == mod) ? reg : REG_NOOP);
    shiftByte((i == mod) ? data : 0x00);
  }
  delayMicroseconds(CLK_DELAY_US);
  digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(CLK_DELAY_US);
}

static uint8_t fb[MODULES][8];

static void flush() {
  for (int m = 0; m < MODULES; m++)
    for (int r = 0; r < 8; r++) maxWrite(m, REG_DIGIT0 + r, fb[m][r]);
}
static void clear() { memset(fb, 0, sizeof(fb)); }

// Screen x -> chain module. With CHAIN_REVERSED the last chain module (the one
// farthest from the ESP32) is the leftmost panel on screen.
static int moduleForX(int x) {
#if CHAIN_REVERSED
  return MODULES - 1 - (x / 8);
#else
  return x / 8;
#endif
}

static void setPixel(int x, int y) {
  if (x < 0 || x >= WIDTH || y < 0 || y > 7) return;
  fb[moduleForX(x)][y] |= (0x80 >> (x % 8));
}

// ---- 5x7 font (column bytes, LSB = top row) --------------------------------
// Index 0 is space; the lookup is by character, so any other symbol shows blank.
static const char FONT_CHARS[] = " -.:%0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t FONT[][5] = {
  {0x00,0x00,0x00,0x00,0x00}, // space
  {0x08,0x08,0x08,0x08,0x08}, // -
  {0x00,0x60,0x60,0x00,0x00}, // .
  {0x00,0x36,0x36,0x00,0x00}, // :
  {0x62,0x64,0x08,0x13,0x23}, // %
  {0x3E,0x51,0x49,0x45,0x3E}, // 0
  {0x00,0x42,0x7F,0x40,0x00}, // 1
  {0x42,0x61,0x51,0x49,0x46}, // 2
  {0x21,0x41,0x45,0x4B,0x31}, // 3
  {0x18,0x14,0x12,0x7F,0x10}, // 4
  {0x27,0x45,0x45,0x45,0x39}, // 5
  {0x3C,0x4A,0x49,0x49,0x30}, // 6
  {0x01,0x71,0x09,0x05,0x03}, // 7
  {0x36,0x49,0x49,0x49,0x36}, // 8
  {0x06,0x49,0x49,0x29,0x1E}, // 9
  {0x7E,0x11,0x11,0x11,0x7E}, // A
  {0x7F,0x49,0x49,0x49,0x36}, // B
  {0x3E,0x41,0x41,0x41,0x22}, // C
  {0x7F,0x41,0x41,0x22,0x1C}, // D
  {0x7F,0x49,0x49,0x49,0x41}, // E
  {0x7F,0x09,0x09,0x09,0x01}, // F
  {0x3E,0x41,0x49,0x49,0x7A}, // G
  {0x7F,0x08,0x08,0x08,0x7F}, // H
  {0x00,0x41,0x7F,0x41,0x00}, // I
  {0x20,0x40,0x41,0x3F,0x01}, // J
  {0x7F,0x08,0x14,0x22,0x41}, // K
  {0x7F,0x40,0x40,0x40,0x40}, // L
  {0x7F,0x02,0x0C,0x02,0x7F}, // M
  {0x7F,0x04,0x08,0x10,0x7F}, // N
  {0x3E,0x41,0x41,0x41,0x3E}, // O
  {0x7F,0x09,0x09,0x09,0x06}, // P
  {0x3E,0x41,0x51,0x21,0x5E}, // Q
  {0x7F,0x09,0x19,0x29,0x46}, // R
  {0x46,0x49,0x49,0x49,0x31}, // S
  {0x01,0x01,0x7F,0x01,0x01}, // T
  {0x3F,0x40,0x40,0x40,0x3F}, // U
  {0x1F,0x20,0x40,0x20,0x1F}, // V
  {0x3F,0x40,0x38,0x40,0x3F}, // W
  {0x63,0x14,0x08,0x14,0x63}, // X
  {0x07,0x08,0x70,0x08,0x07}, // Y
  {0x61,0x51,0x49,0x45,0x43}, // Z
};

static const uint8_t *glyphFor(char ch) {
  ch = (char)toupper((unsigned char)ch);
  const char *p = strchr(FONT_CHARS, ch);
  return p ? FONT[p - FONT_CHARS] : FONT[0];
}

static void drawGlyph(int x, int y, const uint8_t *g) {
  for (int c = 0; c < 5; c++)
    for (int r = 0; r < 7; r++)
      if (g[c] & (1 << r)) setPixel(x + c, y + 1 + r);
}

static void drawText(int x, const char *s) {
  for (const char *p = s; *p; p++) { drawGlyph(x, 0, glyphFor(*p)); x += 6; }
}

static int textWidth(const char *s) { return (int)strlen(s) * 6 - 1; }

// ---- animations -----------------------------------------------------------
static void showCentered(const char *s) {
  clear();
  int x = (WIDTH - textWidth(s)) / 2;
  drawText(x, s);
  flush();
}

static void scrollText(const char *s, int passes, int stepMs) {
  int len = textWidth(s);
  for (int pass = 0; pass < passes; pass++) {
    for (int off = WIDTH; off > -len; off--) {
      clear();
      drawText(off, s);
      flush();
      delay(stepMs);
    }
  }
}

static void blinkText(const char *s, int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    showCentered(s);
    delay(onMs);
    clear(); flush();
    delay(offMs);
  }
}

// Simulated receive level: bars growing from the bottom, 32 columns.
static void levelMeter(int frames, int stepMs) {
  for (int f = 0; f < frames; f++) {
    clear();
    for (int x = 0; x < WIDTH; x++) {
      float phase = (f * 0.35f) + (x * 0.42f);
      int h = 1 + (int)(3.5f * (1.0f + sinf(phase)));
      if (h > 8) h = 8;
      for (int i = 0; i < h; i++) setPixel(x, 7 - i);
    }
    flush();
    delay(stepMs);
  }
}

// A single travelling pixel, used as the "registering" progress indicator.
static void spinner(int frames, int stepMs) {
  for (int f = 0; f < frames; f++) {
    clear();
    drawText((WIDTH - textWidth("REG")) / 2, "REG");
    setPixel(f % WIDTH, 7);             // row 7 sits just under the glyphs
    flush();
    delay(stepMs);
  }
}

static void volumeBar(int pct) {
  clear();
  char label[12];
  snprintf(label, sizeof(label), "VOL %d%%", pct);
  drawText((WIDTH - textWidth(label)) / 2, label);
  flush();
  delay(900);
  // Bar graph along the bottom two rows.
  clear();
  int filled = (WIDTH * pct) / 100;
  for (int x = 0; x < filled; x++) { setPixel(x, 6); setPixel(x, 7); }
  flush();
}

static void maxInit() {
  pinMode(PIN_DIN, OUTPUT);
  pinMode(PIN_CLK, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_DIN, LOW);
  digitalWrite(PIN_CLK, LOW);
  digitalWrite(PIN_CS, HIGH);
  delay(200);

  for (int m = 0; m < MODULES; m++) {
    maxWrite(m, REG_DISPLAYTEST, 0x00);
    maxWrite(m, REG_SHUTDOWN, 0x01);     // leave shutdown FIRST
    maxWrite(m, REG_DECODE, 0x00);
    maxWrite(m, REG_SCANLIMIT, 0x07);
    maxWrite(m, REG_INTENSITY, 0x02);    // low brightness
  }
  clear();
  flush();
}

// ---- the sequence ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("MAX7219 SIP-speaker display sequence");
  Serial.printf("DIN=%d CLK=%d CS=%d modules=%d chain=%s\n",
                PIN_DIN, PIN_CLK, PIN_CS, MODULES,
                CHAIN_REVERSED ? "right-to-left" : "left-to-right");
  maxInit();
  Serial.println("Register init done");

  // [0] Hardware self-test: built-in test register lights every LED regardless
  // of our framebuffer, so it proves power and the data path.
  Serial.println("[0] hardware self-test: built-in display test, all LEDs");
  for (int m = 0; m < MODULES; m++) maxWrite(m, REG_DISPLAYTEST, 0x01);
  delay(1500);
  for (int m = 0; m < MODULES; m++) maxWrite(m, REG_DISPLAYTEST, 0x00);
  clear(); flush();
}

void loop() {
  // [1] Boot
  Serial.println("[1] boot                       <- MAIN: Starting ESP32 SIP Client Application");
  scrollText("ESP32 SIP VOICE", 1, 45);

  // [2] Setup / captive portal
  Serial.println("[2] setup / AP mode            <- WIFI: Starting AP Mode: ESP-SIP-Setup");
  scrollText("AP SETUP 192.168.4.1", 1, 40);
  delay(400);

  // [3] Wi-Fi connecting
  Serial.println("[3] wifi connecting            <- WIFI: Connecting... (retry n/5)");
  blinkText("WIFI", 2, 700, 250);

  // [4] SIP registering
  Serial.println("[4] registering                <- SIP_CLIENT: TX REGISTER / Auth challenge");
  spinner(28, 60);
  delay(300);

  // [5] Registered / idle
  Serial.printf("[5] ready                      <- MAIN: SIP Registered. Application Idle (%s @ %s)\n",
                DEMO_SIP_USER, DEMO_SIP_SERVER);
  char ready[20];
  snprintf(ready, sizeof(ready), "READY %s", DEMO_SIP_USER);
  showCentered(ready);
  for (int i = 0; i < 4; i++) {          // heartbeat pixel in the right corner
    setPixel(WIDTH - 1, 0);
    flush(); delay(280);
    clear(); showCentered(ready); delay(280);
  }

  // [6] Incoming call (speaker mode picks up after the configured delay)
  Serial.println("[6] incoming call              <- SIP_CLIENT: Speaker mode: picking up the call");
  blinkText("RING", 3, 500, 250);
  char caller[20];
  snprintf(caller, sizeof(caller), "CALL %s", DEMO_CALLER);
  scrollText(caller, 1, 55);

  // [7] In call: talk + receive level meter
  Serial.println("[7] in call                    <- AUDIO: Pipeline started: codec PT=0, rate=8000 Hz");
  showCentered("TALK");
  delay(900);
  levelMeter(40, 45);
  clear(); flush();

  // [8] Volume
  Serial.printf("[8] volume                     <- UI_CTRL: Volume -> %d%%\n", DEMO_VOLUME_PCT);
  volumeBar(DEMO_VOLUME_PCT);
  delay(900);

  // [9] Offline
  Serial.println("[9] offline                    <- WIFI: Connection lost. Retrying indefinitely...");
  scrollText("OFFLINE", 1, 55);
  blinkText("X", 3, 350, 200);

  Serial.println("--- sequence complete, repeating ---");
  clear(); flush();
  delay(1200);
}
