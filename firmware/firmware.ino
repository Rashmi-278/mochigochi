// firmware.ino — Tamagotchi firmware entry point (ESP32-C3 SuperMini).
//
// The pet boots into Free Mode and lives on its own (see modes/free_mode):
// it wanders through moods and shows matching expressions. Any serial
// command hands control to Desktop Mode; ~60 s with no command returns it
// to Free Mode. The buzzer is synced to the face — every change jingles in
// Desktop Mode, only mood shifts jingle in Free Mode. See firmware/README.

#include <Wire.h>  // TEMP: I2C bus scan diagnostic — remove after debugging

#include "src/assets/expressions.h"
#include "src/assets/jingles.h"
#include "src/buzzer/buzzer.h"
#include "src/command.h"
#include "src/config.h"
#include "src/modes/desktop_mode.h"
#include "src/modes/free_mode.h"
#include "src/modes/mode.h"
#include "src/mood.h"
#include "src/renderer.h"
#include "src/transport.h"
#include "src/views/view_manager.h"

// The pet's mood — shared between the modes: SET mood writes it, Free Mode
// reads and slowly evolves it. RAM-only (a reboot resets it to content).
static Mood petMood = Mood::Content;

static Renderer renderer;
static Transport transport;
static ViewManager viewManager;
static DesktopMode desktopMode(transport, renderer, petMood);
static FreeMode freeMode(petMood);

// Free Mode is the default; a command switches to Desktop Mode.
static Mode* currentMode = &freeMode;
static uint32_t lastCmdMs = 0;  // time of the last command
static ExpressionId jingledExpr = ExpressionId::Count;

// No serial command for this long, while in Desktop Mode, drifts to Free.
static const uint32_t IDLE_TO_FREE_MS = 60000;

// Hand off between modes via the Mode onExit/onEnter hooks.
static void setMode(Mode& mode) {
  if (currentMode == &mode) return;
  currentMode->onExit();
  currentMode = &mode;
  currentMode->onEnter(viewManager);
}

// Debounced BOOT-button (GPIO9) press detector. Returns true once on each
// release->press edge. Active-low: LOW means pressed.
static bool bootButtonPressed(uint32_t now) {
  static bool raw = false;
  static bool stable = false;
  static uint32_t changeMs = 0;
  bool reading = digitalRead(PIN_BTN_BOOT) == LOW;
  if (reading != raw) {
    raw = reading;
    changeMs = now;
  }
  if (now - changeMs >= 25 && reading != stable) {
    stable = reading;
    if (stable) return true;  // settled into "pressed"
  }
  return false;
}

// TEMP diagnostic: scan the I2C bus once at boot, cache the result, and
// reprint it on a timer from loop(). Reprinting (instead of re-scanning)
// avoids fighting U8g2's HW I2C for the bus, and lets `make monitor`
// catch the result without needing a reset (which drops native USB).
// The OLED should appear at 0x3C. If nothing is found, the panel isn't
// wired/powered correctly (SDA->GPIO5, SCL->GPIO6, VCC->3V3, GND->GND).
static char g_i2cReport[128] = "I2C scan: (not run)";

static void scanI2C() {
  Wire.begin(PIN_SDA, PIN_SCL);
  delay(50);
  int found = 0;
  int n = snprintf(g_i2cReport, sizeof(g_i2cReport), "I2C scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      n += snprintf(g_i2cReport + n, sizeof(g_i2cReport) - n, " 0x%02X", addr);
      found++;
    }
  }
  if (found == 0) snprintf(g_i2cReport + n, sizeof(g_i2cReport) - n, " (none - check wiring/power)");
  Wire.end();
}

void setup() {
  transport.begin(115200);
  delay(300);  // TEMP: let USB CDC settle so the scan output isn't missed
  scanI2C();   // TEMP diagnostic — remove after debugging
  renderer.init();
  buzzer::begin();

  // The on-board BOOT button cycles expressions. The 3 external button
  // pins are wired but unused — configured so they start in a known state.
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);
  pinMode(PIN_BTN_C, INPUT_PULLUP);

  currentMode->onEnter(viewManager);  // boots into Free Mode
  transport.println("Tamagotchi ready (Free Mode). Send any command for Desktop Mode.");
}

void loop() {
  uint32_t now = millis();

  // TEMP diagnostic: reprint the cached I2C scan every 3s so `make monitor`
  // catches it without a reset. Remove with the rest of the scan code.
  static uint32_t lastI2cPrintMs = 0;
  if (now - lastI2cPrintMs >= 3000) {
    lastI2cPrintMs = now;
    Serial.println(g_i2cReport);
  }

  Command cmd;
  if (transport.poll(cmd)) {
    lastCmdMs = now;
    setMode(desktopMode);  // any command means a host is driving the pet
    currentMode->onCommand(cmd, viewManager);
  }

  // BOOT button: a tap cycles to the next expression (and, like a
  // command, hands control to Desktop Mode).
  if (bootButtonPressed(now)) {
    lastCmdMs = now;
    setMode(desktopMode);
    uint8_t next = (static_cast<uint8_t>(viewManager.face().expression()) + 1) % expressionCount();
    viewManager.face().setExpression(static_cast<ExpressionId>(next));
  }

  if (currentMode == &desktopMode && now - lastCmdMs >= IDLE_TO_FREE_MS) {
    setMode(freeMode);  // gone idle — let the pet live on its own
  }

  currentMode->update(now, viewManager);

  // Buzzer synced to the face. In Desktop Mode every expression change
  // jingles; Free Mode stays quieter and plays its own jingle only on
  // mood shifts, so it is not gated in here.
  ExpressionId expr = viewManager.face().expression();
  if (expr != jingledExpr) {
    jingledExpr = expr;
    if (currentMode == &desktopMode) {
      Jingle jingle = jingleFor(expr);
      buzzer::play(jingle.tones, jingle.count);
    }
  }
  buzzer::update(now);

  renderer.beginFrame();
  viewManager.tick(now, renderer);
  renderer.endFrame();
}
