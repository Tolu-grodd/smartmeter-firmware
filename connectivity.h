#ifndef CONNECTIVITY_H
#define CONNECTIVITY_H

#include <Arduino.h>
#include "config.h"
#include "power_calc.h"

// ============================================================
//  SETTINGS received from broker — passed to taskEnergy
// ============================================================
struct ConnectivitySettings {
  float   voltage_V;
  float   topup_kwh;
  float   set_units_kwh;
  char    phone[32];
  uint8_t conn_mode;
  char    wifi_ssid[64];
  char    wifi_password[64];
  bool    has_update;
};

// ============================================================
//  PUBLIC API
// ============================================================
void connectivity_init(const char* device_id);
void connectivity_loop();
bool connectivity_publish(PowerData data);
bool connectivity_publishAlert(bool low_units, bool critical_units);
bool connectivity_isConnected();
void connectivity_getSettings(ConnectivitySettings &settings);
void connectivity_setMode(uint8_t mode);

// ============================================================
//  OTA STATE — readable by taskEnergy
//
//  _ota_in_progress: true while binary is downloading.
//    taskEnergy checks this to suppress alert publishes and
//    avoid competing for the network stack mid-download.
//    Set/cleared only from taskConnectivity (Core 1).
//    Read from taskEnergy (Core 0) — volatile, no mutex needed
//    for a single bool read (atomic on Xtensa).
// ============================================================
extern volatile bool connectivity_ota_in_progress;

#endif