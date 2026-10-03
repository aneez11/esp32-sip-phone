// =====================================================================
//  Pin probe — find out which ESP pin reaches which module pin
// =====================================================================
//
// Toggles GPIO 10, 11, 12 one at a time (HIGH for 5 s, then LOW for 5 s)
// and says which one is active on the serial console. Put a multimeter
// (or an LED+resistor) between GND and each module input pin, and watch
// which ESP pin makes it move.
//
// Expected mapping for MAX7219:
//   ESP 10 -> DIN
//   ESP 11 -> CLK
//   ESP 12 -> CS / LOAD
//
// Pins: DIN=10 CLK=11 CS=12 (ESP32-S3). VCC 3V3, common GND.
// =====================================================================

#include <Arduino.h>

static const int PINS[3] = {10, 11, 12};
static const char *NAMES[3] = {"DIN (GPIO10)", "CLK (GPIO11)", "CS (GPIO12)"};

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== ESP32-S3 MAX7219 pin probe ===");
  Serial.println("Each ESP pin goes HIGH for 5s, then LOW for 5s.");
  Serial.println("Probe the module's DIN / CLK / CS pins with a meter to GND.");
  for (int i = 0; i < 3; i++) {
    pinMode(PINS[i], OUTPUT);
    digitalWrite(PINS[i], LOW);
  }
  Serial.println("Ready.");
}

void loop() {
  for (int i = 0; i < 3; i++) {
    Serial.printf(">>> %s : HIGH\n", NAMES[i]);
    digitalWrite(PINS[i], HIGH);
    delay(5000);
    Serial.printf(">>> %s : LOW\n", NAMES[i]);
    digitalWrite(PINS[i], LOW);
    delay(5000);
  }
  Serial.println("--- repeat ---");
}
