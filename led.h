#ifndef LED_H
#define LED_H

#include <Arduino.h>
#include "config.h"

// ============================================================
//  LED MODES
//  Solid on     — connected and running normally
//  Slow 1Hz     — connecting / waiting
//  Fast 5Hz     — error state
//  Double blink — low units alert active
//  Off          — explicitly off
// ============================================================
enum LedMode {
  LED_OFF,
  LED_SOLID,
  LED_SLOW,
  LED_FAST,
  LED_DOUBLE_BLINK
};

void led_init();
void led_update();
void led_setMode(LedMode mode);

#endif