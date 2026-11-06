#ifndef POWER_CALC_H
#define POWER_CALC_H

#include <Arduino.h>
#include "config.h"

// ============================================================
//  POWER DATA — final struct for all hardware phases
//
//  Phase 1 (ADC/CT clamp): rms_current_A, voltage_V, power_W live
//    power_factor = 1.0f (assumed), frequency_Hz = 50.0f (assumed)
//    apparent_power_VA = power_W (same as real power when PF=1)
//    mains_present = always true
//
//  Phase 2 (PZEM): all fields live and measured
//    mains_present = false when PZEM times out
//
//  Phase 3 (ATM90E26): all fields live, higher accuracy
//    same struct, ATM90E26 fills it over SPI
// ============================================================
struct PowerData {
  // --- Electrical measurements ---
  float rms_current_A;        // Measured current
  float voltage_V;            // Phase 1: assumed 220V | Phase 2+: measured
  float power_W;              // Real (active) power
  float power_factor;         // Phase 1: 1.0f assumed | Phase 2+: measured
  float frequency_Hz;         // Phase 1: 50.0f assumed | Phase 2+: measured
  float apparent_power_VA;    // Phase 1: = power_W | Phase 2+: measured

  // --- Energy accumulation ---
  float energy_Wh;            // Session energy (resets on restart)
  float total_energy_kWh;     // Cumulative lifetime energy (NVS persisted)

  // --- Units budget ---
  float units_purchased_kWh;  // What user paid for
  float units_remaining;      // units_purchased - total_energy_kWh

  // --- Status ---
  bool  mains_present;        // Phase 1: always true | Phase 2: PZEM timeout = false
  uint32_t mains_on_duration_s;  // Seconds current mains-on run has been going. Resets to 0 on mains loss.
  bool  low_units_alert;      // units_remaining <= LOW_UNITS_THRESHOLD
  bool  critical_units_alert; // units_remaining <= CRITICAL_UNITS_THRESHOLD
};

// ============================================================
//  PUBLIC API
// ============================================================
void      powerCalc_init(float voltage);
void      powerCalc_sample();
PowerData powerCalc_getData();
void      powerCalc_setVoltage(float voltage_V);
void      powerCalc_setUnitsBudget(float units_kWh);
bool      powerCalc_checkAlert();
void      powerCalc_resetSession();
void      powerCalc_setTotalEnergy(float total_kWh);
bool      powerCalc_isMainsPresent();

#endif