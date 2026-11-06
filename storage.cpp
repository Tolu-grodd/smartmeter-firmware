#include "storage.h"
#include <Preferences.h>

static Preferences _prefs;

#define NVS_NAMESPACE       "smartmeter"

// NVS keys
#define KEY_TOTAL_ENERGY    "total_kwh"
#define KEY_UNITS_BUDGET    "units_budget"
#define KEY_VOLTAGE         "voltage"
#define KEY_PHONE           "phone"
#define KEY_CONN_MODE       "conn_mode"
#define KEY_WIFI_SSID       "wifi_ssid"
#define KEY_WIFI_PASS       "wifi_pass"
#define KEY_FW_VERSION      "fw_version"
#define KEY_DEVICE_ID       "device_id"
#define KEY_GSM_APN         "gsm_apn"
#define KEY_OTA_PENDING     "ota_pending"   // 1 = device rebooted into OTA, needs validation

// ============================================================
//  INIT
// ============================================================
void storage_init() {
  _prefs.begin(NVS_NAMESPACE, false);

  Serial.println("[Storage] NVS initialised");
  Serial.print("[Storage] Total energy:  ");
  Serial.print(_prefs.getFloat(KEY_TOTAL_ENERGY, 0.0f), 4);
  Serial.println(" kWh");
  Serial.print("[Storage] Units budget:  ");
  Serial.print(_prefs.getFloat(KEY_UNITS_BUDGET, 0.0f), 2);
  Serial.println(" kWh");
  Serial.print("[Storage] Voltage:       ");
  Serial.print(_prefs.getFloat(KEY_VOLTAGE, DEFAULT_VOLTAGE_V), 1);
  Serial.println(" V");
  Serial.print("[Storage] Phone:         ");
  Serial.println(_prefs.getString(KEY_PHONE, "not set"));
  Serial.print("[Storage] Conn mode:     ");
  Serial.println(_prefs.getUChar(KEY_CONN_MODE, DEFAULT_CONN_MODE) == CONN_MODE_WIFI
                 ? "WiFi" : "GSM");
  Serial.print("[Storage] WiFi SSID:     ");
  Serial.println(_prefs.getString(KEY_WIFI_SSID, "not set"));
  Serial.print("[Storage] GSM APN:       ");
  Serial.println(_prefs.getString(KEY_GSM_APN, "not set (will cycle on first connect)"));
  Serial.print("[Storage] Device ID:     ");
  Serial.println(_prefs.getString(KEY_DEVICE_ID, "not set"));
  Serial.print("[Storage] FW version:    ");
  Serial.println(_prefs.getString(KEY_FW_VERSION, "not set"));
  Serial.print("[Storage] OTA pending:   ");
  Serial.println(_prefs.getUChar(KEY_OTA_PENDING, 0) ? "YES — validation required" : "no");

  _prefs.end();
}

// ============================================================
//  TOTAL ENERGY
// ============================================================
void storage_saveTotalEnergy(float total_kWh) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putFloat(KEY_TOTAL_ENERGY, total_kWh);
  _prefs.end();
}

float storage_loadTotalEnergy() {
  _prefs.begin(NVS_NAMESPACE, true);
  float val = _prefs.getFloat(KEY_TOTAL_ENERGY, 0.0f);
  _prefs.end();
  return val;
}

// ============================================================
//  UNITS BUDGET
// ============================================================
void storage_saveUnitsBudget(float units_kWh) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putFloat(KEY_UNITS_BUDGET, units_kWh);
  _prefs.end();
  Serial.print("[Storage] Units budget saved: ");
  Serial.println(units_kWh);
}

float storage_loadUnitsBudget() {
  _prefs.begin(NVS_NAMESPACE, true);
  float val = _prefs.getFloat(KEY_UNITS_BUDGET, 0.0f);
  _prefs.end();
  return val;
}

// ============================================================
//  VOLTAGE
// ============================================================
void storage_saveVoltage(float voltage_V) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putFloat(KEY_VOLTAGE, voltage_V);
  _prefs.end();
  Serial.print("[Storage] Voltage saved: ");
  Serial.println(voltage_V);
}

float storage_loadVoltage() {
  _prefs.begin(NVS_NAMESPACE, true);
  float val = _prefs.getFloat(KEY_VOLTAGE, DEFAULT_VOLTAGE_V);
  _prefs.end();
  return val;
}

// ============================================================
//  PHONE NUMBER
// ============================================================
void storage_savePhoneNumber(const char* phone) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putString(KEY_PHONE, phone);
  _prefs.end();
  Serial.print("[Storage] Phone saved: ");
  Serial.println(phone);
}

String storage_loadPhoneNumber() {
  _prefs.begin(NVS_NAMESPACE, true);
  String val = _prefs.getString(KEY_PHONE, "");
  _prefs.end();
  return val;
}

// ============================================================
//  CONNECTIVITY MODE
// ============================================================
void storage_saveConnMode(uint8_t mode) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putUChar(KEY_CONN_MODE, mode);
  _prefs.end();
  Serial.print("[Storage] Conn mode saved: ");
  Serial.println(mode == CONN_MODE_WIFI ? "WiFi" : "GSM");
}

uint8_t storage_loadConnMode() {
  _prefs.begin(NVS_NAMESPACE, true);
  uint8_t val = _prefs.getUChar(KEY_CONN_MODE, DEFAULT_CONN_MODE);
  _prefs.end();
  return val;
}

// ============================================================
//  WIFI CREDENTIALS
// ============================================================
void storage_saveWifiCredentials(const char* ssid, const char* password) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putString(KEY_WIFI_SSID, ssid);
  _prefs.putString(KEY_WIFI_PASS, password);
  _prefs.end();
  Serial.print("[Storage] WiFi credentials saved. SSID: ");
  Serial.println(ssid);
}

String storage_loadWifiSSID() {
  _prefs.begin(NVS_NAMESPACE, true);
  String val = _prefs.getString(KEY_WIFI_SSID, "");
  _prefs.end();
  return val;
}

String storage_loadWifiPassword() {
  _prefs.begin(NVS_NAMESPACE, true);
  String val = _prefs.getString(KEY_WIFI_PASS, "");
  _prefs.end();
  return val;
}

// ============================================================
//  FIRMWARE VERSION
// ============================================================
void storage_saveFwVersion(const char* version) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putString(KEY_FW_VERSION, version);
  _prefs.end();
}

String storage_loadFwVersion() {
  _prefs.begin(NVS_NAMESPACE, true);
  String val = _prefs.getString(KEY_FW_VERSION, "");
  _prefs.end();
  return val;
}

// ============================================================
//  DEVICE ID
// ============================================================
void storage_saveDeviceID(const char* device_id) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putString(KEY_DEVICE_ID, device_id);
  _prefs.end();
  Serial.print("[Storage] Device ID saved: ");
  Serial.println(device_id);
}

String storage_loadDeviceID() {
  _prefs.begin(NVS_NAMESPACE, true);
  String val = _prefs.getString(KEY_DEVICE_ID, "");
  _prefs.end();
  return val;
}

// ============================================================
//  GSM APN
// ============================================================
void storage_saveGsmApn(const char* apn) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putString(KEY_GSM_APN, apn);
  _prefs.end();
  Serial.print("[Storage] GSM APN saved: ");
  Serial.println(apn);
}

String storage_loadGsmApn() {
  _prefs.begin(NVS_NAMESPACE, true);
  String val = _prefs.getString(KEY_GSM_APN, "");
  _prefs.end();
  return val;
}

// ============================================================
//  OTA PENDING FLAG
//
//  Written to 1 just before esp_restart() at end of OTA download.
//  Read on boot — if 1, connectivity task enters validation mode:
//    connect to MQTT within OTA_VALIDATION_WINDOW_MS → mark valid + clear flag
//    fail to connect within window → flag stays, rollback on next reboot
//  Cleared explicitly after successful validation.
//  Cleared on factory reset.
// ============================================================
void storage_saveOtaPending(uint8_t pending) {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.putUChar(KEY_OTA_PENDING, pending);
  _prefs.end();
  Serial.print("[Storage] OTA pending flag: ");
  Serial.println(pending ? "SET" : "CLEARED");
}

uint8_t storage_loadOtaPending() {
  _prefs.begin(NVS_NAMESPACE, true);
  uint8_t val = _prefs.getUChar(KEY_OTA_PENDING, 0);
  _prefs.end();
  return val;
}

// ============================================================
//  FACTORY RESET
// ============================================================
void storage_reset() {
  _prefs.begin(NVS_NAMESPACE, false);
  _prefs.clear();
  _prefs.end();
  Serial.println("[Storage] All NVS data wiped — factory reset");
}