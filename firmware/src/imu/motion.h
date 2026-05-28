// motion.h — turn an accel/gyro stream into discrete pet-relevant events.
//
// The pet only cares about two gestures right now:
//
//   Pickup — being lifted off the desk. Looks like sustained |a| above
//            ~1.25 g held for ~250 ms (someone is accelerating it upward
//            and gravity isn't being cancelled out).
//
//   Shake  — rapid back-and-forth. Looks like several large peaks of
//            opposite sign on any axis within a short window.
//
// The detector is intentionally hysteretic: after firing an event it
// stays quiet for a refractory period so a single pickup doesn't fire
// repeatedly as the user adjusts their grip.
//
// All numeric thresholds are tuned conservatively — false negatives are
// much less annoying than a pet that "panics" every time the desk shakes.
#pragma once

#include <stdint.h>

#include "mpu6050.h"

namespace motion {

enum class Event : uint8_t {
  None,
  Pickup,
  Shake,
};

// Initialise the detector. Safe to call multiple times.
void begin();

// Feed one fresh sample. Returns the event (often None). `now` is millis().
Event update(const imu::Sample& s, uint32_t now);

}  // namespace motion
