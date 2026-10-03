// =====================================================================
//  MAX7219 8x8 x4 chain — hardware test sketch (ESP32-S3)
// =====================================================================
//
// Standalone bring-up test. Runs the display in a sequence and prints what
// each step should look like, so a partial/garbled result is diagnosable.
//
// Pins (match ESP32-SIP-Voice defaults):
//     DIN  = GPIO 10     VCC = 5V
//     CLK  = GPIO 11     GND = GND
//     CS   = GPIO 12
//
// Wiring rules for a chain of N modules:
//     ESP DIN -> module1 DIN
//     module1 DOUT -> module2 DIN -> module3 DIN -> module4 DIN
//     CLK and CS go to ALL modules in parallel.
//
// If it still flickers with no pattern, the usual causes are:
//   * DIN/CLK swapped, or DIN wired to a DOUT pin
//   * modules not sharing a common ground with the ESP32
//   * 5V supply too weak / not 5V (MAX7219 needs ~5V; 3.3V is marginal)
//   * one module in the chain is faulty (try a single module first)
// =====================================================================

#include <Arduino.h>

static const int PIN_DIN = 10;
static const int PIN_CLK = 11;
static const int PIN_CS  = 12;

static const int MODULES = 4;
static const int WIDTH   = MODULES * 8;

// Bit-bang SPI at a deliberately slow, reliable rate (MAX7219 tolerates up
// to 10 MHz, but slow rules out signal-integrity issues during bring-up).
#define CLK_DELAY_US 5

#define REG_NOOP        0x00
#define REG_DIGIT0      0x01
#define REG_DECODE      0x09
#define REG_INTENSITY   0x0A
#define REG_SCANLIMIT   0x0B
#define REG_SHUTDOWN    0x0C
#define REG_DISPLAYTEST 0x0F

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

// ---- 5x7 font (column bytes, LSB = top row) ----
static const uint8_t G_SPACE[5] = {0x00,0x00,0x00,0x00,0x00};
static const uint8_t G_T[5]     = {0x01,0x01,0x7F,0x01,0x01};
static const uint8_t G_E[5]     = {0x7F,0x49,0x49,0x49,0x41};
static const uint8_t G_S[5]     = {0x46,0x49,0x49,0x49,0x31};
static const uint8_t G_1[5]     = {0x00,0x42,0x7F,0x40,0x00};
static const uint8_t G_2[5]     = {0x42,0x61,0x51,0x49,0x46};
static const uint8_t G_3[5]     = {0x21,0x41,0x45,0x4B,0x31};
static const uint8_t G_4[5]     = {0x18,0x14,0x12,0x7F,0x10};

static const uint8_t *glyphFor(char ch) {
  switch (ch) {
    case 'T': return G_T;
    case 'E': return G_E;
    case 'S': return G_S;
    case '1': return G_1;
    case '2': return G_2;
    case '3': return G_3;
    case '4': return G_4;
    default:  return G_SPACE;
  }
}

static uint8_t fb[MODULES][8];

static void flush() {
  for (int m = 0; m < MODULES; m++)
    for (int r = 0; r < 8; r++) maxWrite(m, REG_DIGIT0 + r, fb[m][r]);
}
static void clear() { memset(fb, 0, sizeof(fb)); }
static void setPixel(int x, int y) {
  if (x < 0 || x >= WIDTH || y < 0 || y > 7) return;
  fb[x / 8][y] |= (0x80 >> (x % 8));
}
static void drawGlyph(int x, int y, const uint8_t *g) {
  for (int c = 0; c < 5; c++)
    for (int r = 0; r < 7; r++)
      if (g[c] & (1 << r)) setPixel(x + c, y + 1 + r);
}
static void drawText(int x, const char *s) {
  for (const char *p = s; *p; p++) { drawGlyph(x, 0, glyphFor(*p)); x += 6; }
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

// Step 0 — the MAX7219 built-in test register: every LED on, full brightness,
// independent of our framebuffer. If this lights the whole chain, the data
// path and power are good and any garbage later is a framing bug.
static void stepDisplayTestOn()  { for (int m=0;m<MODULES;m++) maxWrite(m, REG_DISPLAYTEST, 0x01); }
static void stepDisplayTestOff() { for (int m=0;m<MODULES;m++) maxWrite(m, REG_DISPLAYTEST, 0x00); }

static void stepAllOn() {
  for (int m=0;m<MODULES;m++) for (int r=0;r<8;r++) fb[m][r]=0xFF;
  flush();
}

// Fill ONE module at a time so we can see which position in the chain works.
static void stepOneModuleAtATime() {
  for (int m = 0; m < MODULES; m++) {
    clear();
    for (int r = 0; r < 8; r++) fb[m][r] = 0xFF;
    flush();
    delay(700);
  }
}

static void stepDigits() {
  clear();
  drawGlyph(2, 0, G_1); drawGlyph(10, 0, G_2);
  drawGlyph(18, 0, G_3); drawGlyph(26, 0, G_4);
  flush();
  delay(1500);
}

static void stepScroll() {
  const char *msg = "TEST  ";
  int len = strlen(msg) * 6;
  for (int off = WIDTH; off > -len; off--) {
    clear();
    drawText(off, msg);
    flush();
    delay(50);
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("MAX7219 bring-up test");
  Serial.printf("DIN=%d CLK=%d CS=%d modules=%d clk_delay=%dus\n",
                PIN_DIN, PIN_CLK, PIN_CS, MODULES, CLK_DELAY_US);
  maxInit();
  Serial.println("Register init done");
}

void loop() {
  Serial.println("Step 1: built-in display test (all LEDs on)");
  stepDisplayTestOn();  delay(1200);
  stepDisplayTestOff(); delay(400);

  Serial.println("Step 2: all pixels on");
  stepAllOn(); delay(800);

  Serial.println("Step 3: one module at a time (1..4)");
  stepOneModuleAtATime();

  Serial.println("Step 4: digits 1234, one per module");
  stepDigits();

  Serial.println("Step 5: scroll TEST");
  stepScroll();

  Serial.println("--- repeat ---");
  delay(500);
}
