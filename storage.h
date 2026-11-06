#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include "config.h"

// ============================================================
//  All NVS read/write goes through this module only
//  No other file calls Preferences directly
// ============================================================

void    storage_init();

// --- Energy ---
void    storage_saveTotalEnergy(float total_kWh);
float   storage_loadTotalEnergy();

// --- Units budget ---
void    storage_saveUnitsBudget(float units_kWh);
float   storage_loadUnitsBudget();

// --- Voltage ---
void    storage_saveVoltage(float voltage_V);
float   storage_loadVoltage();

// --- Phone number ---
void    storage_savePhoneNumber(const char* phone);
String  storage_loadPhoneNumber();

// --- Connectivity mode ---
void    storage_saveConnMode(uint8_t mode);
uint8_t storage_loadConnMode();

// --- WiFi credentials ---
void    storage_saveWifiCredentials(const char* ssid, const char* password);
String  storage_loadWifiSSID();
String  storage_loadWifiPassword();

// --- Firmware version ---
void    storage_saveFwVersion(const char* version);
String  storage_loadFwVersion();

// --- Device ID (MAC-based, saved once on first boot) ---
void    storage_saveDeviceID(const char* device_id);
String  storage_loadDeviceID();

// --- GSM APN (saved after first successful GPRS connect) ---
void    storage_saveGsmApn(const char* apn);
String  storage_loadGsmApn();

// --- OTA pending flag ---
//  Set to 1 before initiating OTA reboot.
//  Read on boot by connectivity task to know it must validate
//  the new firmware by reaching MQTT within OTA_VALIDATION_WINDOW_MS.
//  Cleared after successful validation (mark_valid called).
//  Also cleared on factory reset.
void    storage_saveOtaPending(uint8_t pending);
uint8_t storage_loadOtaPending();

// --- Factory reset — wipes all keys ---
void    storage_reset();

#endif