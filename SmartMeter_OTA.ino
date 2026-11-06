#include "config.h"
#include "power_calc.h"
#include "storage.h"
#include "connectivity.h"
#if BLE_PROVISION_ENABLED
#include "ble_provision.h"
#endif
#include "ap_provision.h"
#include "led.h"
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <WiFi.h>
#include <esp_mac.h>

// ============================================================
//  OTA ROLLBACK ENABLE
//
//  This must be set for esp_ota_mark_app_valid_cancel_rollback()
//  to have any effect. Without it, every OTA boot is auto-validated
//  immediately and rollback never fires.
//
//  In Arduino IDE this is controlled via sdkconfig / menuconfig.
//  The cleanest way to force it on is via a sdkconfig.defaults
//  file in the sketch folder containing:
//    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
//
//  If you do not have that file yet, create it now alongside this
//  sketch. Arduino IDE picks it up on next compile.
//  Without it, OTA will still work — flashing and rebooting — but
//  the rollback safety net will be silently absent.
// ============================================================

// ============================================================
//  DEVICE ID
// ============================================================
static char _device_id[20] = {0};

static void _generate_device_id() {
  String saved = storage_loadDeviceID();
  if (saved.length() > 0) {
    strncpy(_device_id, saved.c_str(), sizeof(_device_id) - 1);
    Serial.print("[Setup] Device ID loaded from NVS: ");
    Serial.println(_device_id);
    return;
  }

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(_device_id, sizeof(_device_id),
           "SM-%02X%02X%02X", mac[3], mac[4], mac[5]);

  storage_saveDeviceID(_device_id);
  Serial.print("[Setup] Device ID generated: ");
  Serial.println(_device_id);
}

// ============================================================
//  SHARED STATE
// ============================================================
static SemaphoreHandle_t _data_mutex;
static PowerData         _shared_data;

// ============================================================
//  SETTINGS QUEUE — Core 1 → Core 0
// ============================================================
struct SettingsMsg {
  float   voltage;
  float   topup_kwh;
  float   set_units_kwh;
  char    phone[32];
  uint8_t conn_mode;
  char    wifi_ssid[64];
  char    wifi_password[64];
  bool    has_conn_update;
};
static QueueHandle_t _settings_queue;

// ============================================================
//  SHARED ALERT FLAGS
// ============================================================
static volatile bool _alert_pending = false;
static volatile bool _alert_fired   = false;

// ============================================================
//  REPROVISION FLAG
// ============================================================
static volatile bool _reprovision_requested = false;

// ============================================================
//  SUPERCAP POWER FAIL ISR
// ============================================================
static volatile bool _power_fail_flag = false;

#ifdef POWER_FAIL_ENABLED
void IRAM_ATTR onPowerFail() {
  _power_fail_flag = true;
}
#endif

// ============================================================
//  CORE 0 TASK — Energy sampling, NVS, alert detection
// ============================================================
void taskEnergy(void* pvParameters) {
  Serial.print("[Energy] Task started on core: ");
  Serial.println(xPortGetCoreID());

  float voltage         = storage_loadVoltage();
  float units_purchased = storage_loadUnitsBudget();
  float saved_energy    = storage_loadTotalEnergy();

  if (units_purchased == 0.0f) {
    units_purchased = DEFAULT_UNITS_PURCHASED_KWH;
    storage_saveUnitsBudget(units_purchased);
    Serial.println("[Energy] First boot — default units set");
  }

  powerCalc_init(voltage);
  if (saved_energy > 0.0f) powerCalc_setTotalEnergy(saved_energy);
  powerCalc_setUnitsBudget(units_purchased);

  uint32_t last_nvs_save    = millis();
  uint32_t last_sample_time = millis();
  uint32_t boot_time        = millis();
  bool     boot_phase       = true;

  esp_task_wdt_add(NULL);

  static uint32_t _hold_start   = 0;
  static bool     _holding      = false;
  static bool     _grace_done   = false;
  static bool     _prompt_shown = false;

  for (;;) {

    // ---- Power fail handler ----
#if defined(POWER_FAIL_ENABLED)
    if (_power_fail_flag) {
      _power_fail_flag = false;

#elif defined(POWER_FAIL_PZEM_ENABLED)
    {
      static bool _last_mains = true;
      bool _mains_now = powerCalc_isMainsPresent();
      bool _trigger   = (_last_mains == true && _mains_now == false);
      _last_mains     = _mains_now;
      if (_trigger) {

#else
    if (false) {
#endif

      led_setMode(LED_OFF);
      PowerData d = powerCalc_getData();
      storage_saveTotalEnergy(d.total_energy_kWh);
      storage_saveUnitsBudget(units_purchased);
      Serial.println("[Energy] *** POWER FAIL — NVS saved ***");
      Serial.print("[Energy]     total:  ");
      Serial.print(d.total_energy_kWh, 4);
      Serial.println(" kWh");
      Serial.print("[Energy]     units:  ");
      Serial.println(units_purchased, 4);
     }
    }

    // ---- Boot button hold logic ----
    if (!_grace_done) {
      if (millis() > 3000) {
        _grace_done = true;
      }
    } else {

      if (digitalRead(FACTORY_RESET_PIN) == LOW) {

        if (!_holding) {
          _holding      = true;
          _hold_start   = millis();
          _prompt_shown = false;
          Serial.println("[Energy] Button held — hold 3s to re-provision, 10s for factory reset");
        }

        uint32_t held_ms = millis() - _hold_start;

        if (held_ms >= FACTORY_RESET_HOLD_MS) {
          Serial.println("[Energy] FACTORY RESET CONFIRMED — wiping NVS");
          led_setMode(LED_FAST);
          delay(1000);
          storage_reset();
          esp_restart();

        } else if (held_ms >= REPROVISION_HOLD_MS && !_prompt_shown) {
          _prompt_shown = true;
          led_setMode(LED_DOUBLE_BLINK);
          Serial.println("[Energy] Release now to re-provision WiFi");
          Serial.println("[Energy] Keep holding for full factory reset (10s)");
        }

      } else {

        if (_holding) {
          uint32_t held_ms = millis() - _hold_start;

          if (held_ms >= REPROVISION_HOLD_MS) {
            Serial.println("[Energy] Re-provisioning triggered — rebooting into setup mode");
            storage_saveWifiCredentials("", "");
            storage_saveConnMode(CONN_MODE_WIFI);
            led_setMode(LED_FAST);
            delay(500);
            esp_restart();

          } else {
            Serial.println("[Energy] Button released — cancelled");
            led_setMode(connectivity_isConnected() ? LED_SOLID : LED_SLOW);
          }

          _holding      = false;
          _prompt_shown = false;
        }
      }
    }

    // ---- Settings from Core 1 ----
    SettingsMsg msg;
    if (xQueueReceive(_settings_queue, &msg, 0) == pdTRUE) {

      if (msg.voltage > 0.0f && msg.voltage != voltage) {
        voltage = msg.voltage;
        powerCalc_setVoltage(voltage);
        storage_saveVoltage(voltage);
      }

      if (msg.set_units_kwh > 0.0f) {
        units_purchased = msg.set_units_kwh;
        powerCalc_setUnitsBudget(units_purchased);
        storage_saveUnitsBudget(units_purchased);
        _alert_fired = false;
        Serial.print("[Energy] Units SET to: ");
        Serial.println(units_purchased);
      } else if (msg.topup_kwh > 0.0f) {
        units_purchased += msg.topup_kwh;
        powerCalc_setUnitsBudget(units_purchased);
        storage_saveUnitsBudget(units_purchased);
        _alert_fired = false;
        Serial.print("[Energy] Units topped up — total: ");
        Serial.println(units_purchased);
      }

      if (strlen(msg.phone) > 0) {
        storage_savePhoneNumber(msg.phone);
        Serial.print("[Energy] Phone updated: ");
        Serial.println(msg.phone);
      }

      if (msg.has_conn_update) {
        storage_saveConnMode(msg.conn_mode);
        if (strlen(msg.wifi_ssid) > 0) {
          storage_saveWifiCredentials(msg.wifi_ssid, msg.wifi_password);
        }
        Serial.println("[Energy] Connectivity settings saved");
      }
    }

    // ---- Sample power ----
    powerCalc_sample();
    PowerData data = powerCalc_getData();

    // ---- Write shared data under mutex ----
    if (xSemaphoreTake(_data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      _shared_data = data;
      xSemaphoreGive(_data_mutex);
    }

    // ---- Alert detection ----
    // Skip alert flagging during OTA — taskConnectivity won't publish it anyway,
    // but avoid touching _alert_pending while the network stack is busy.
    if (!connectivity_ota_in_progress) {
      if (powerCalc_checkAlert() && !_alert_fired) {
        _alert_pending = true;
        _alert_fired   = true;
        Serial.println("[Energy] LOW UNITS ALERT set");
      }

      if (data.units_remaining > LOW_UNITS_THRESHOLD + 2.0f) {
        _alert_fired = false;
      }
    }

    // ---- NVS save ----
    uint32_t save_interval = boot_phase ? BOOT_SAVE_INTERVAL_MS : NVS_SAVE_INTERVAL_MS;

    if (millis() - last_nvs_save >= save_interval) {
      last_nvs_save = millis();
      storage_saveTotalEnergy(data.total_energy_kWh);
      Serial.print("[Energy] NVS save — total: ");
      Serial.print(data.total_energy_kWh, 4);
      Serial.print(" kWh");
      if (boot_phase) Serial.print(" [boot phase]");
      Serial.println();
    }

    if (boot_phase && millis() - boot_time >= BOOT_SAVE_DURATION_MS) {
      boot_phase = false;
      Serial.println("[Energy] Boot phase complete — normal NVS save interval");
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    // ---- Sample gap watchdog ----
    uint32_t gap = millis() - last_sample_time;
    last_sample_time = millis();
    if (gap > CORE_SAMPLE_WATCHDOG_MS) {
      Serial.print("[Energy] WARNING — sample gap: ");
      Serial.print(gap);
      Serial.println("ms");
    }

    esp_task_wdt_reset();
  }
}

// ============================================================
//  CORE 1 TASK — Connectivity, publish, settings relay
// ============================================================
void taskConnectivity(void* pvParameters) {
  Serial.print("[Connect] Task started on core: ");
  Serial.println(xPortGetCoreID());

  connectivity_init(_device_id);

  uint32_t last_upload    = millis();
  bool     wdt_registered = false;

  for (;;) {

    if (!wdt_registered && connectivity_isConnected()) {
      esp_task_wdt_add(NULL);
      wdt_registered = true;
      Serial.println("[Connect] Watchdog registered");
    }

    connectivity_loop();

    // Skip settings relay and publish during OTA
    if (!connectivity_ota_in_progress) {

      ConnectivitySettings s;
      connectivity_getSettings(s);

      if (s.has_update) {
        SettingsMsg msg;
        memset(&msg, 0, sizeof(msg));
        msg.voltage       = s.voltage_V;
        msg.topup_kwh     = s.topup_kwh;
        msg.set_units_kwh = s.set_units_kwh;
        strncpy(msg.phone, s.phone, sizeof(msg.phone) - 1);
        msg.conn_mode     = s.conn_mode;
        strncpy(msg.wifi_ssid, s.wifi_ssid, sizeof(msg.wifi_ssid) - 1);
        strncpy(msg.wifi_password, s.wifi_password, sizeof(msg.wifi_password) - 1);
        msg.has_conn_update = (s.conn_mode != 0 || strlen(s.wifi_ssid) > 0);

        xQueueSend(_settings_queue, &msg, pdMS_TO_TICKS(10));
        Serial.println("[Connect] Settings queued for Core 0");

        if (msg.has_conn_update) {
          connectivity_setMode(s.conn_mode);
        }
      }

      if (millis() - last_upload >= UPLOAD_INTERVAL_MS) {
        last_upload = millis();

        PowerData data;
        if (xSemaphoreTake(_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          data = _shared_data;
          xSemaphoreGive(_data_mutex);
        }

        connectivity_publish(data);

        if (_alert_pending) {
          connectivity_publishAlert(data.low_units_alert,
                                    data.critical_units_alert);
          _alert_pending = false;
          Serial.println("[Connect] Alert published");
        }
      }

      if (connectivity_isConnected()) {
        if (_shared_data.low_units_alert) {
          led_setMode(LED_DOUBLE_BLINK);
        } else {
          led_setMode(LED_SOLID);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    if (wdt_registered) esp_task_wdt_reset();
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("========================================");
  Serial.println("    Smart Energy Meter");
  Serial.println("    ESP32 Classic | FW v" FW_VERSION);
  Serial.println("========================================");

  led_init();
  led_setMode(LED_SLOW);

  storage_init();
  storage_saveFwVersion(FW_VERSION);

  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

  _generate_device_id();

  String  saved_ssid = storage_loadWifiSSID();
  uint8_t conn_mode  = storage_loadConnMode();

  bool needs_provisioning = (saved_ssid.length() == 0 &&
                              conn_mode == CONN_MODE_WIFI);

  if (needs_provisioning) {
  #if BLE_PROVISION_ENABLED
      Serial.println("[Setup] No credentials — starting BLE + AP provisioning");
      bleProvision_start(_device_id);
      apProvision_start(_device_id);

      while (true) {
        bleProvision_loop();
        apProvision_loop();
        led_update();

        if (bleProvision_isComplete()) {
          Serial.println("[Setup] BLE provisioning completed");
          bleProvision_stop();
          apProvision_stop();
          break;
        }

        if (apProvision_isComplete()) {
          Serial.println("[Setup] AP provisioning completed");
          apProvision_stop();
          bleProvision_stop();
          break;
        }

        if (bleProvision_timedOut() && apProvision_timedOut()) {
          Serial.println("[Setup] Both BLE and AP timed out — using hardcoded defaults");
          apProvision_stop();
          bleProvision_stop();
          break;
        }

        delay(10);
      }

  #else
      Serial.println("[Setup] No credentials — starting AP provisioning");
      apProvision_start(_device_id);

      while (true) {
        apProvision_loop();
        led_update();

        if (apProvision_isComplete()) {
          Serial.println("[Setup] AP provisioning completed");
          apProvision_stop();
          break;
        }

        if (apProvision_timedOut()) {
          Serial.println("[Setup] AP timed out — using hardcoded defaults");
          apProvision_stop();
          break;
        }

        delay(10);
      }
  #endif

  } else {
    Serial.println("[Setup] Credentials found in NVS — skipping provisioning");
  }

#ifdef POWER_FAIL_ENABLED
  pinMode(POWER_FAIL_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(POWER_FAIL_PIN),
                  onPowerFail, FALLING);
  Serial.println("[Setup] Power fail ISR attached");
#else
  Serial.println("[Setup] Power fail ISR — disabled (uncomment POWER_FAIL_ENABLED when wired)");
#endif

  _data_mutex = xSemaphoreCreateMutex();
  if (_data_mutex == NULL) {
    Serial.println("[Setup] FATAL — mutex creation failed");
    while (1);
  }

  _settings_queue = xQueueCreate(4, sizeof(SettingsMsg));
  if (_settings_queue == NULL) {
    Serial.println("[Setup] FATAL — queue creation failed");
    while (1);
  }

  xTaskCreatePinnedToCore(
    taskEnergy, "EnergyTask",
    STACK_ENERGY, NULL, PRIORITY_ENERGY, NULL, CORE_ENERGY
  );

  xTaskCreatePinnedToCore(
    taskConnectivity, "ConnectTask",
    STACK_CONNECT, NULL, PRIORITY_CONNECT, NULL, CORE_CONNECTIVITY
  );

  Serial.println("[Setup] Tasks created — FreeRTOS scheduler running");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  vTaskDelete(NULL);
}
