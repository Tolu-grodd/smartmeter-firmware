#include "led.h"

static LedMode   _mode          = LED_OFF;
static uint32_t  _last_toggle   = 0;
static bool      _led_state     = false;
static uint8_t   _blink_count   = 0;
static uint32_t  _phase_start   = 0;

void led_init() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.println("[LED] Initialised");
}

void led_setMode(LedMode mode) {
  if (_mode == mode) return;
  _mode        = mode;
  _blink_count = 0;
  _last_toggle = millis();
  _phase_start = millis();

  // Immediately apply solid/off
  if (mode == LED_SOLID) {
    digitalWrite(LED_PIN, HIGH);
    _led_state = true;
  } else if (mode == LED_OFF) {
    digitalWrite(LED_PIN, LOW);
    _led_state = false;
  }
}

void led_update() {
  uint32_t now = millis();

  switch (_mode) {

    case LED_SOLID:
    case LED_OFF:
      // Set once in led_setMode — nothing to do here
      break;

    case LED_SLOW: {
      // 1Hz — 500ms on, 500ms off
      if (now - _last_toggle >= 500) {
        _last_toggle = now;
        _led_state   = !_led_state;
        digitalWrite(LED_PIN, _led_state ? HIGH : LOW);
      }
      break;
    }

    case LED_FAST: {
      // 5Hz — 100ms on, 100ms off
      if (now - _last_toggle >= 100) {
        _last_toggle = now;
        _led_state   = !_led_state;
        digitalWrite(LED_PIN, _led_state ? HIGH : LOW);
      }
      break;
    }

    case LED_DOUBLE_BLINK: {
      // Two quick blinks then pause — repeating
      // Pattern: on 100ms, off 100ms, on 100ms, off 700ms = 1 second cycle
      uint32_t cycle = (now - _phase_start) % 1000;
      bool new_state;
      if      (cycle < 100)  new_state = true;
      else if (cycle < 200)  new_state = false;
      else if (cycle < 300)  new_state = true;
      else                   new_state = false;

      if (new_state != _led_state) {
        _led_state = new_state;
        digitalWrite(LED_PIN, _led_state ? HIGH : LOW);
      }
      break;
    }
  }
}