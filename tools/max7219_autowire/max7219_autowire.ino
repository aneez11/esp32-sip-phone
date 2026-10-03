// =====================================================================
//  MAX7219 — display-test latch check across ALL pin mappings
// =====================================================================
//
// For each of the 6 assignments of GPIO {10,11,12} to DIN/CLK/CS:
//   * turn the built-in display test ON  (should be all LEDs lit)
//   * turn it OFF                        (should go dark)
// If a mapping makes the panel go dark, that mapping latches -> correct.
// If NO mapping ever goes dark, no frame is latching:
//     -> check the board is on its INPUT header, and
//     -> try powering the board from 5 V (most 4-in-1 boards expect 5 V).
//
// Each phase is 3 s ON + 3 s OFF so it is easy to watch.
// =====================================================================

#include <Arduino.h>

#define DLY 25
#define R_DIGIT0      0x01
#define R_DECODE      0x09
#define R_INTENSITY   0x0A
#define R_SCANLIMIT   0x0B
#define R_SHUTDOWN    0x0C
#define R_DISPLAYTEST 0x0F

static int g_din, g_clk, g_cs;

static void pushByte(uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(g_din, (b >> i) & 1);
    delayMicroseconds(DLY);
    digitalWrite(g_clk, HIGH);
    delayMicroseconds(DLY);
    digitalWrite(g_clk, LOW);
    delayMicroseconds(DLY);
  }
}
static void cmd(uint8_t reg, uint8_t data) {
  digitalWrite(g_cs, LOW);
  delayMicroseconds(DLY);
  pushByte(reg); pushByte(data);
  delayMicroseconds(DLY);
  digitalWrite(g_cs, HIGH);
  delayMicroseconds(DLY);
}
static void initFor(int din, int clk, int cs) {
  g_din = din; g_clk = clk; g_cs = cs;
  pinMode(din, OUTPUT); pinMode(clk, OUTPUT); pinMode(cs, OUTPUT);
  digitalWrite(din, LOW); digitalWrite(clk, LOW); digitalWrite(cs, HIGH);
  cmd(R_SHUTDOWN, 0x01);
  cmd(R_DECODE, 0x00);
  cmd(R_SCANLIMIT, 0x07);
  cmd(R_INTENSITY, 0x08);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== MAX7219 display-test latch check ===");
}

void loop() {
  int p[3] = {10, 11, 12};
  int perms[6][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
  for (int k = 0; k < 6; k++) {
    int din = p[perms[k][0]], clk = p[perms[k][1]], cs = p[perms[k][2]];
    Serial.printf("\n--- DIN=%d CLK=%d CS=%d ---\n", din, clk, cs);
    initFor(din, clk, cs);

    Serial.println("display-test ON  -> expect ALL LEDs lit");
    cmd(R_DISPLAYTEST, 0x01);
    delay(3000);

    Serial.println("display-test OFF -> expect DARK");
    cmd(R_DISPLAYTEST, 0x00);
    for (int r = 0; r < 8; r++) cmd(R_DIGIT0 + r, 0x00);
    delay(3000);
  }
  Serial.println("\n=== repeat ===");
}
