#ifndef AP_PROVISION_H
#define AP_PROVISION_H

#include <Arduino.h>
#include "config.h"

// ============================================================
//  AP PROVISIONING — captive portal
//
//  Flow:
//    1. Call apProvision_start(device_id)
//       → starts WiFi AP named after device ID (e.g. "SM-D3BEFC")
//       → starts DNS server redirecting all traffic to 192.168.4.1
//       → starts web server serving config form
//    2. Call apProvision_loop() repeatedly alongside bleProvision_loop()
//    3. Customer connects phone to "SM-XXXXXX" WiFi (open, no password)
//    4. Browser opens automatically (captive portal) or customer opens any URL
//    5. Customer fills form: WiFi SSID, password, phone number (optional)
//    6. apProvision_isComplete() returns true
//    7. Call apProvision_stop() then bleProvision_stop()
//
//  No SPIFFS needed — HTML is a string literal in firmware
//  Runs in parallel with BLE — whichever completes first wins
// ============================================================

void apProvision_start(const char* device_id);
void apProvision_loop();
void apProvision_stop();
bool apProvision_isComplete();
bool apProvision_timedOut();

#endif