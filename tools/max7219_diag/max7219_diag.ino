// =====================================================================
//  MAX7219 — CS / data-reach probe (single module)
// =====================================================================
//
// Symptom this targets: module scans, all LEDs lit, never follows commands.
// That means the chip is not latching our frames. This sketch makes the
// difference between "chip in display-test mode" and "commands not arriving"
// obvious, then walks a single pixel.
//
// Pins: DIN=10 CLK=11 CS=12   (ESP32-S3)
// VCC 3V3, common GND.
// =====================================================================

#include <Arduino.h>

static const int PIN_DIN = 10;
static const int PIN_CLK = 11;
static const int PIN_CS  = 12;

#define DLY 20            // microseconds, very slow

#define R_NOOP        0x00
#define R_DIGIT0      0x01
#define R_DECODE      0x09
#define R_INTENSITY   0x0A
#define R_SCANLIMIT   0x0B
#define R_SHUTDOWN    0x0C
#define R_DISPLAYTEST 0x0F

static void pushByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_DIN, (b >> i) & 1);
    delayMicroseconds(DLY);
    digitalWrite(PIN_CLK, HIGH);
    delayMicroseconds(DLY);
    digitalWrite(PIN_CLK, LOW);
    delayMicroseconds(DLY);
  }
}

static void cmd(uint8_t reg, uint8_t data) {
  digitalWrite(PIN_CS, LOW);
  delayMicroseconds(DLY);
  pushByte(reg);
  pushByte(data);
  delayMicroseconds(DLY);
  digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(DLY);
}

// Some clone boards latch on the RISING edge of LOAD, others on the falling.
// This variant shifts while CS is HIGH then pulses CS LOW (alternative frame),
// to see whether edge polarity is the issue.
static void cmdAltEdge(uint8_t reg, uint8_t data) {
  digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(DLY);
  pushByte(reg);
  pushByte(data);
  delayMicroseconds(DLY);
  digitalWrite(PIN_CS, LOW);
  digitalWrite(PIN_CS, HIGH);
  delayMicroseconds(DLY);
}

static void allRows(uint8_t v) {
  for (int r = 0; r < 8; r++) cmd(R_DIGIT0 + r, v);
}

static void initNormal() {
  cmd(R_DISPLAYTEST, 0x00);
  cmd(R_SHUTDOWN, 0x01);
  cmd(R_DECODE, 0x00);
  cmd(R_SCANLIMIT, 0x07);
  cmd(R_INTENSITY, 0x00);
  allRows(0x00);
}

// Walk a single pixel: if this shows a moving dot, data is arriving.
static void walkPixel() {
  for (int col = 0; col < 8; col++) {
    uint8_t mask = 0x80 >> col;
    for (int r = 0; r < 8; r++) cmd(R_DIGIT0 + r, (r == 0) ? mask : 0x00);
    delay(300);
  }
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("MAX7219 CS/data probe");
  Serial.printf("DIN=%d CLK=%d CS=%d\n", PIN_DIN, PIN_CLK, PIN_CS);

  pinMode(PIN_DIN, OUTPUT);
  pinMode(PIN_CLK, OUTPUT);
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_DIN, LOW);
  digitalWrite(PIN_CLK, LOW);
  digitalWrite(PIN_CS, HIGH);
  delay(250);

  // 1) Force display-test ON, then OFF immediately. If it stays fully lit
  //    after OFF, the chip never got the OFF frame.
  Serial.println("Phase 1: display-test ON then OFF");
  cmd(R_DISPLAYTEST, 0x01); delay(1000);
  cmd(R_DISPLAYTEST, 0x00); delay(1000);

  Serial.println("Phase 2: normal init + blank");
  initNormal(); delay(1000);

  Serial.println("Phase 3: single row at a time (top->bottom)");
  allRows(0x00);
  for (int r = 0; r < 8; r++) {
    cmd(R_DIGIT0 + r, 0xFF);
    delay(500);
    cmd(R_DIGIT0 + r, 0x00);
  }

  Serial.println("Phase 4: walk a single pixel");
  allRows(0x00);
  walkPixel();
}

void loop() {
  Serial.println("Phase 3: single row at a time");
  allRows(0x00);
  for (int r = 0; r < 8; r++) {
    cmd(R_DIGIT0 + r, 0xFF);
    delay(600);
    cmd(R_DIGIT0 + r, 0x00);
  }

  Serial.println("Phase 4: walk pixel");
  allRows(0x00);
  walkPixel();

  Serial.println("Phase 5: alternate-edge framing test");
  cmdAltEdge(R_SHUTDOWN, 0x01);
  cmdAltEdge(R_DISPLAYTEST, 0x00);
  allRows(0x00);
  cmdAltEdge(R_DIGIT0 + 3, 0xFF);   // middle row only
  delay(1500);
  allRows(0x00);
}

