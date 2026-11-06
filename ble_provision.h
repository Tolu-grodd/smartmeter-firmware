#ifndef BLE_PROVISION_H
#define BLE_PROVISION_H

#include <Arduino.h>
#include "config.h"

// ============================================================
//  BLE PROVISIONING
//
//  Flow:
//    1. Call bleProvision_start(device_id) — begins advertising
//    2. Call bleProvision_loop() repeatedly until complete or timed out
//    3. User connects via nRF Connect and writes JSON:
//       {"ssid":"YourNetwork","password":"YourPass","conn_mode":0}
//    4. bleProvision_isComplete() returns true
//    5. Call bleProvision_stop() before starting WiFi
//
//  conn_mode in JSON: 0 = WiFi, 1 = GSM
//  If conn_mode omitted, defaults to CONN_MODE_WIFI
// ============================================================

void bleProvision_start(const char* device_id);
void bleProvision_loop();
void bleProvision_stop();
bool bleProvision_isComplete();
bool bleProvision_isActive();
bool bleProvision_timedOut();

#endif