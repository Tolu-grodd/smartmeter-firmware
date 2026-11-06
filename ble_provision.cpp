#include "ble_provision.h"
#include "storage.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>

static BLEServer*           _ble_server     = nullptr;
static BLECharacteristic*   _char_provision = nullptr;
static BLECharacteristic*   _char_status    = nullptr;

static bool     _is_active   = false;
static bool     _is_complete = false;
static bool     _timed_out   = false;
static uint32_t _start_time  = 0;

// ============================================================
//  BLE SERVER CALLBACKS — connection events
// ============================================================
class ProvisionServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    Serial.println("[BLE] Client connected");
  }
  void onDisconnect(BLEServer* server) override {
    Serial.println("[BLE] Client disconnected");
    if (_is_active && !_is_complete) {
      BLEDevice::startAdvertising();
    }
  }
};

// ============================================================
//  PROVISION CHARACTERISTIC CALLBACKS — receives credentials
//
//  Expected JSON payload from Flutter app or nRF Connect:
//  {
//    "ssid":     "YourNetwork",
//    "password": "YourPass",
//    "phone":    "08012345678",   ← optional, SMS alerts
//    "conn_mode": 0               ← 0=WiFi, 1=GSM. Defaults to WiFi if omitted
//  }
// ============================================================
class ProvisionCharCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String value = characteristic->getValue().c_str();

    Serial.print("[BLE] Received: ");
    Serial.println(value);

    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, value);

    if (err) {
      Serial.println("[BLE] JSON parse error — ignoring");
      if (_char_status) {
        _char_status->setValue("ERROR: invalid JSON");
        _char_status->notify();
      }
      return;
    }

    const char* ssid     = doc["ssid"]      | "";
    const char* password = doc["password"]  | "";
    const char* phone    = doc["phone"]     | "";
    uint8_t     mode     = doc["conn_mode"] | (uint8_t)DEFAULT_CONN_MODE;

    if (strlen(ssid) == 0) {
      Serial.println("[BLE] No SSID in payload — ignoring");
      if (_char_status) {
        _char_status->setValue("ERROR: missing ssid");
        _char_status->notify();
      }
      return;
    }

    // Save credentials to NVS
    storage_saveWifiCredentials(ssid, password);
    storage_saveConnMode(mode);

    // Phone number — save only if provided, empty is fine
    if (strlen(phone) > 0) {
      storage_savePhoneNumber(phone);
      Serial.print("[BLE] Phone saved: ");
      Serial.println(phone);
    }

    Serial.print("[BLE] Credentials saved. SSID: ");
    Serial.print(ssid);
    Serial.print(" | Mode: ");
    Serial.println(mode == CONN_MODE_WIFI ? "WiFi" : "GSM");

    if (_char_status) {
      _char_status->setValue("OK: credentials saved");
      _char_status->notify();
    }

    _is_complete = true;
  }
};

// ============================================================
//  PUBLIC API
// ============================================================
void bleProvision_start(const char* device_id) {
  Serial.println("[BLE] Starting provisioning mode");

  _is_active   = true;
  _is_complete = false;
  _timed_out   = false;
  _start_time  = millis();

  String ble_name = String(BLE_DEVICE_PREFIX) + String(device_id).substring(5);

  BLEDevice::init(ble_name.c_str());
  _ble_server = BLEDevice::createServer();
  _ble_server->setCallbacks(new ProvisionServerCallbacks());

  BLEService* service = _ble_server->createService(BLE_SERVICE_UUID);

  _char_provision = service->createCharacteristic(
    BLE_CHAR_PROVISION_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  _char_provision->setCallbacks(new ProvisionCharCallbacks());

  _char_status = service->createCharacteristic(
    BLE_CHAR_STATUS_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  _char_status->addDescriptor(new BLE2902());
  _char_status->setValue("WAITING");

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.print("[BLE] Advertising as: ");
  Serial.println(ble_name);
  Serial.println("[BLE] Send JSON: {\"ssid\":\"...\",\"password\":\"...\",\"phone\":\"...\",\"conn_mode\":0}");
  Serial.print("[BLE] Timeout in ");
  Serial.print(BLE_TIMEOUT_MS / 1000);
  Serial.println("s");
}

void bleProvision_loop() {
  if (!_is_active || _is_complete) return;

  if (millis() - _start_time >= BLE_TIMEOUT_MS) {
    Serial.println("[BLE] Provisioning timed out — falling back to defaults");
    _timed_out = true;
    _is_active = false;

    storage_saveWifiCredentials(WIFI_SSID_DEFAULT, WIFI_PASSWORD_DEFAULT);
    storage_saveConnMode(DEFAULT_CONN_MODE);

    Serial.println("[BLE] Default credentials saved to NVS");
  }
}

void bleProvision_stop() {
  if (!_is_active && !_is_complete && !_timed_out) return;

  Serial.println("[BLE] Stopping BLE");
  BLEDevice::stopAdvertising();
  BLEDevice::deinit(true);
  delay(500);

  _ble_server     = nullptr;
  _char_provision = nullptr;
  _char_status    = nullptr;
  _is_active      = false;

  Serial.println("[BLE] BLE stopped — radio free for WiFi");
}

bool bleProvision_isComplete() { return _is_complete; }
bool bleProvision_isActive()   { return _is_active; }
bool bleProvision_timedOut()   { return _timed_out; }