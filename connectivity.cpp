// ============================================================
//  connectivity.cpp — Smart Energy Meter
//
//  Dual-mode connectivity: WiFi + GSM/A7670E
//  Mode stored in NVS — respects user choice, no auto-fallback.
//
//  WiFi path : VPS Mosquitto, TLS port 8883, WiFiClientSecure (mbedTLS)
//  GSM path  : VPS Mosquitto, TLS port 8883, TinyGsmClientSecure
//              (A7670E native TLS — bench-proven, see MIGRATION
//              NOTES in config.h for why this replaced SIM800L)
//
//  OTA:
//    Trigger — backend publishes to smartmeter/ota:
//      { "ota_version": "1.1.0", "ota_url": "https://..." }
//    Device compares version, downloads only if newer.
//    Chunked esp_https_ota_perform() loop feeds watchdog every chunk.
//
//    *** WiFi OTA works as before. GSM OTA DOES NOT WORK YET ***
//    esp_https_ota() uses the ESP32's own network stack, which has
//    no visibility into the A7670E's cellular connection. This is
//    a separate, unsolved problem — see config.h OTA section and
//    the project handoff doc. _ota_do_update() below will currently
//    fail cleanly on GSM (esp_https_ota_begin will fail to resolve/
//    connect, since there's no active ESP32-side network interface
//    when in GSM mode) — it does not crash, it just can't succeed
//    until PPP or a manual AT-driven download path is implemented.
//
//    After OTA reboot: device has OTA_VALIDATION_WINDOW_MS to reach
//    MQTT. Success → mark_valid + clear NVS flag. Timeout → rollback
//    on next reboot via ESP-IDF bootloader mechanism.
// ============================================================

#define TINY_GSM_MODEM_A76XXSSL          // A7670E native SSL socket support
#define TINY_GSM_USE_GPRS   true
#define TINY_GSM_USE_WIFI   false
#define TINY_GSM_RX_BUFFER  1024         // A76xx benefits from a larger RX buffer

#include "connectivity.h"
#include "storage.h"
#include "led.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <TinyGsmClient.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp_https_ota.h>
#include <esp_http_client.h>
#include "esp_crt_bundle.h"
// NOTE: SSLClient.h and certificates.h are NO LONGER USED on the
// GSM path — the A7670E does native TLS and takes a cert FILENAME
// (see GSM_CACERT_FILENAME in config.h), not the BearSSL binary
// trust-anchor format certificates.h provided for SSLClient.
// certificates.h can be deleted once you've confirmed nothing else
// references it. Left both includes out entirely below.

// ============================================================
//  MQTT CA CERTIFICATE — ISRG Root X1 (Let's Encrypt)
//  Used by WiFiClientSecure (WiFi path only) to verify the broker.
//  GSM path uses GSM_CACERT_FILENAME instead — see _gsm_ensure_cert_loaded().
//  Broker: your MQTT broker (MQTT_BROKER in config.h)
//  Do not use setInsecure() in production — always verify.
// ============================================================
static const char* _mqtt_ca_cert = \
"-----BEGIN CERTIFICATE-----\n" \
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n" \
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n" \
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n" \
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n" \
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n" \
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n" \
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n" \
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n" \
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n" \
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n" \
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n" \
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n" \
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n" \
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n" \
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n" \
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n" \
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n" \
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n" \
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n" \
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n" \
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n" \
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n" \
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n" \
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n" \
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n" \
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n" \
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n" \
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n" \
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n" \
"-----END CERTIFICATE-----\n";

// ============================================================
//  GSM BROKER
// ============================================================
#define GSM_MQTT_BROKER   MQTT_BROKER
#define GSM_MQTT_PORT     8883

// ============================================================
//  DEVICE IDENTITY
// ============================================================
static char _device_id[20]      = {0};
static char _topic_readings[64] = {0};
static char _topic_settings[64] = {0};
static char _topic_alerts[64]   = {0};
static char _mqtt_client_id[32] = {0};

// ============================================================
//  CONNECTIVITY STATE
// ============================================================
static uint8_t  _conn_mode     = CONN_MODE_WIFI;
static bool     _ntp_synced    = false;
static uint32_t _last_ntp_sync = 0;

// ============================================================
//  OTA STATE
// ============================================================
volatile bool connectivity_ota_in_progress = false;

// Pending OTA request — written by MQTT callback, consumed by connectivity_loop()
static bool _ota_requested        = false;
static char _ota_url[OTA_URL_MAX_LEN]         = {0};
static char _ota_version[OTA_VERSION_MAX_LEN] = {0};

// Validation window — started on boot if NVS ota_pending == 1
static bool     _ota_validation_active     = false;
static uint32_t _ota_validation_start_ms   = 0;
static bool     _ota_validated             = false;   // true once mark_valid called this boot

// ============================================================
//  SETTINGS
// ============================================================
static ConnectivitySettings _pending_settings;
static bool                 _settings_ready = false;

// ============================================================
//  VERSION COMPARISON
// ============================================================
static bool _ota_version_is_newer(const char* incoming, const char* running) {
  int inc_maj = 0, inc_min = 0, inc_pat = 0;
  int run_maj = 0, run_min = 0, run_pat = 0;

  sscanf(incoming, "%d.%d.%d", &inc_maj, &inc_min, &inc_pat);
  sscanf(running,  "%d.%d.%d", &run_maj, &run_min, &run_pat);

  if (inc_maj != run_maj) return inc_maj > run_maj;
  if (inc_min != run_min) return inc_min > run_min;
  return inc_pat > run_pat;
}

// ============================================================
//  MQTT CALLBACK — shared between WiFi and GSM paths
//  UNCHANGED from SIM800L version. Bench-confirmed working
//  correctly over the A7670E's native SSL socket via
//  TinyGsmClientSecure + PubSubClient (11+ consecutive publishes
//  and a live subscribe/callback firing mid-session, verified
//  against the real broker on 2026-06-19).
// ============================================================
static void _mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("[Connect] MQTT received | Topic: ");
  Serial.print(topic);
  Serial.print(" | Payload: ");
  Serial.println(message);

  StaticJsonDocument<320> doc;
  if (deserializeJson(doc, message)) {
    Serial.println("[Connect] JSON parse error");
    return;
  }

  // ---- OTA trigger — check before settings parsing ----
  if (doc.containsKey("ota_version") && doc.containsKey("ota_url")) {
    const char* incoming_ver = doc["ota_version"].as<const char*>();
    const char* url          = doc["ota_url"].as<const char*>();

    if (!incoming_ver || !url || strlen(url) == 0) {
      Serial.println("[OTA] Malformed trigger — missing version or URL");
      return;
    }

    if (!_ota_version_is_newer(incoming_ver, FW_VERSION)) {
      Serial.print("[OTA] Ignoring — incoming ");
      Serial.print(incoming_ver);
      Serial.print(" is not newer than running ");
      Serial.println(FW_VERSION);
      return;
    }

    if (connectivity_ota_in_progress) {
      Serial.println("[OTA] Already in progress — ignoring duplicate trigger");
      return;
    }

    strncpy(_ota_version, incoming_ver, sizeof(_ota_version) - 1);
    strncpy(_ota_url,     url,          sizeof(_ota_url)     - 1);
    _ota_requested = true;

    Serial.print("[OTA] Update queued — version: ");
    Serial.print(_ota_version);
    Serial.print(" | URL: ");
    Serial.println(_ota_url);
    return;  // Do not process as settings
  }

  // ---- Regular settings ----
  memset(&_pending_settings, 0, sizeof(_pending_settings));
  _pending_settings.has_update = false;

  if (doc.containsKey("voltage_V")) {
    _pending_settings.voltage_V  = doc["voltage_V"].as<float>();
    _pending_settings.has_update = true;
  }
  if (doc.containsKey("topup_kWh")) {
    float v = doc["topup_kWh"].as<float>();
    if (v > 0.0f) {
      _pending_settings.topup_kwh  = v;
      _pending_settings.has_update = true;
    }
  }
  if (doc.containsKey("set_units_kWh")) {
    float v = doc["set_units_kWh"].as<float>();
    if (v > 0.0f) {
      _pending_settings.set_units_kwh = v;
      _pending_settings.has_update    = true;
    }
  }
  if (doc.containsKey("phone_number")) {
    strncpy(_pending_settings.phone,
            doc["phone_number"].as<const char*>(),
            sizeof(_pending_settings.phone) - 1);
    _pending_settings.has_update = true;
  }
  if (doc.containsKey("conn_mode")) {
    _pending_settings.conn_mode  = doc["conn_mode"].as<uint8_t>();
    _pending_settings.has_update = true;
  }
  if (doc.containsKey("wifi_ssid") && doc.containsKey("wifi_password")) {
    strncpy(_pending_settings.wifi_ssid,
            doc["wifi_ssid"].as<const char*>(),
            sizeof(_pending_settings.wifi_ssid) - 1);
    strncpy(_pending_settings.wifi_password,
            doc["wifi_password"].as<const char*>(),
            sizeof(_pending_settings.wifi_password) - 1);
    _pending_settings.has_update = true;
  }

  if (_pending_settings.has_update) {
    _settings_ready = true;
  }
}

// ============================================================
//  NTP
// ============================================================
static void _sync_ntp(const char* ntp_server) {
  Serial.print("[Connect] Syncing NTP via ");
  Serial.println(ntp_server);
  configTime(NTP_GMT_OFFSET_S, NTP_DST_OFFSET_S, ntp_server);

  struct tm timeinfo;
  uint32_t  start = millis();
  while (!getLocalTime(&timeinfo) && millis() - start < 15000) {
    esp_task_wdt_reset();
    delay(200);
  }

  if (getLocalTime(&timeinfo)) {
    _ntp_synced    = true;
    _last_ntp_sync = millis();
    Serial.print("[Connect] NTP synced: ");
    Serial.println(&timeinfo, "%Y-%m-%dT%H:%M:%S");
  } else {
    Serial.println("[Connect] NTP sync failed — will retry");
  }
}

// ------------------------------------------------------------
//  GSM TIME — reads time from the cellular network directly via
//  the modem's own AT+CCLK, instead of configTime()/NTP-over-UDP.
//
//  WHY: configTime()/getLocalTime() are built around the ESP32's
//  own WiFi/lwIP network stack. They have no path to reach the
//  A7670E's cellular data connection at all — there is no "modem
//  aware" NTP in the ESP32 core. Confirmed by bench testing:
//  configTime() fails 100% of the time over GSM regardless of
//  which NTP server is configured (tried time.google.com AND
//  pool.ntp.org — both fail identically), while the exact same
//  function works fine over WiFi. This is a mechanism problem,
//  not a server problem.
//
//  Uses TinyGSM's built-in modem.getNetworkTime(), which sends
//  AT+CCLK? and parses the network-provided time/date/timezone.
//  This depends on the carrier actually populating network time
//  (MTN — untested at the time this was written; some carriers
//  disable this at the network level, per community reports).
//
//  DEFENSIVE HANDLING (a known TinyGSM issue: getNetworkTime can
//  return a crash-inducing malformed string, and separately a
//  known "return year 1939 / epoch" bug on rare occasions):
//    - Return value is checked before use.
//    - Year is sanity-checked (2020-2099) before being trusted;
//      obviously-wrong values (1939, 1970, 2004 factory-default
//      etc.) are rejected rather than applied.
//    - Uses ESP32's own setTime()-equivalent (via `struct tm` +
//      the standard time.h `settimeofday()`) to push the modem's
//      time into the same system clock that getLocalTime() reads,
//      so _get_iso_timestamp() doesn't need to change at all —
//      it keeps reading the normal system clock either way.
// ------------------------------------------------------------
// (moved below — see _gsm_sync_time_from_network() in the GSM
// section, after _modem is declared)

static String _get_iso_timestamp() {
  if (!_ntp_synced) return "1970-01-01T00:00:00+01:00";
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01T00:00:00+01:00";
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+01:00", &timeinfo);
  return String(buf);
}

// ============================================================
//  OTA DOWNLOAD — chunked breathing loop
//
//  *** GSM PATH: DOES NOT WORK YET — see config.h OTA section ***
//  esp_https_ota() only sees the ESP32's native network interfaces
//  (WiFi/Ethernet). It has no path to the A7670E's cellular link.
//  On GSM this will fail at esp_https_ota_begin() and log an error
//  — it will NOT crash or corrupt anything, it will just report
//  failure and connectivity_loop() resumes normal operation, same
//  as any other OTA failure path. Left unmodified from the SIM800L
//  version because the fix (PPP or manual AT-driven download) is
//  independent of the SIM800L->A7670E migration and is tracked
//  separately. WiFi OTA is fully unaffected.
// ============================================================
// Forward declaration — actual implementation is in the GSM section
// below (needs _modem/SerialGSM, declared later in this file).
static bool _gsm_ota_do_update(const char* url, const char* version);

static void _ota_do_update(const char* url, const char* version) {
  Serial.println("[OTA] ========================================");
  Serial.print("[OTA] Starting download — version: ");
  Serial.println(version);
  Serial.print("[OTA] URL: ");
  Serial.println(url);
  Serial.print("[OTA] Free heap before OTA: ");
  Serial.println(esp_get_free_heap_size());

  if (_conn_mode == CONN_MODE_GSM) {
    // GSM path: esp_https_ota() cannot see the A7670E's cellular
    // link (it only knows the ESP32's own WiFi/Ethernet stack).
    // Use the A7670E's native HTTP(S) AT commands instead — bench-
    // verified working (real ~1.24MB firmware file downloaded via
    // AT+HTTPACTION/AT+HTTPREAD, HTTPS + cert verification, ~10-15s).
    // See _gsm_ota_do_update() for the implementation and its
    // "not yet bench-tested AS FIRMWARE" caveat.
    _gsm_ota_do_update(url, version);
    return;
  }

  Serial.println("[OTA] Chunked loop active — watchdog fed every chunk");
  Serial.println("[OTA] ========================================");

  connectivity_ota_in_progress = true;
  led_setMode(LED_FAST);

  // Configure HTTP client
  esp_http_client_config_t http_config = {};
  http_config.url             = url;
  http_config.timeout_ms      = 30000;   // 30s per-chunk TCP timeout
  http_config.keep_alive_enable = true;
  http_config.crt_bundle_attach     = esp_crt_bundle_attach;
  http_config.buffer_size           = 4096;
  http_config.buffer_size_tx        = 1024;
  http_config.max_redirection_count = 3;

  esp_https_ota_config_t ota_config = {};
  ota_config.http_config = &http_config;

  // Feed watchdog before TLS handshake — GSM can take 30-60s to establish
  esp_task_wdt_reset();

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);

  if (err != ESP_OK || ota_handle == NULL) {
    Serial.print("[OTA] Begin failed: ");
    Serial.println(esp_err_to_name(err));
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return;
  }

  Serial.println("[OTA] Connection established — downloading...");

  int     chunk_count    = 0;
  int32_t image_size     = esp_https_ota_get_image_size(ota_handle);
  int32_t bytes_written  = 0;

  if (image_size > 0) {
    Serial.print("[OTA] Binary size: ");
    Serial.print(image_size);
    Serial.println(" bytes");
  }

  // ---- Breathing loop ----
  while (true) {
    err = esp_https_ota_perform(ota_handle);

    if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      esp_task_wdt_reset();
      chunk_count++;
      bytes_written = esp_https_ota_get_image_len_read(ota_handle);

      if (chunk_count % OTA_PROGRESS_LOG_CHUNKS == 0) {
        if (image_size > 0) {
          int pct = (int)((bytes_written * 100L) / image_size);
          Serial.print("[OTA] Progress: ");
          Serial.print(pct);
          Serial.print("% (");
          Serial.print(bytes_written);
          Serial.print("/");
          Serial.print(image_size);
          Serial.println(" bytes)");
        } else {
          Serial.print("[OTA] Downloaded: ");
          Serial.print(bytes_written);
          Serial.println(" bytes");
        }
      }
      continue;
    }

    if (err == ESP_OK) {
      esp_task_wdt_reset();
      bytes_written = esp_https_ota_get_image_len_read(ota_handle);
      Serial.print("[OTA] Download complete — ");
      Serial.print(bytes_written);
      Serial.println(" bytes written");
      break;
    }

    Serial.print("[OTA] Download error: ");
    Serial.println(esp_err_to_name(err));
    esp_https_ota_abort(ota_handle);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    Serial.println("[OTA] Incomplete download — aborting");
    esp_https_ota_abort(ota_handle);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return;
  }

  err = esp_https_ota_finish(ota_handle);
  if (err != ESP_OK) {
    Serial.print("[OTA] Finish/validation failed: ");
    Serial.println(esp_err_to_name(err));
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return;
  }

  storage_saveOtaPending(1);

  Serial.println("[OTA] ========================================");
  Serial.println("[OTA] Flash complete — rebooting into new firmware");
  Serial.print("[OTA] New version: ");
  Serial.println(version);
  Serial.println("[OTA] Must connect to MQTT within validation window to confirm.");
  Serial.println("[OTA] ========================================");

  delay(1000);
  esp_restart();
}

// ============================================================
//  OTA VALIDATION — unchanged
// ============================================================
static void _ota_check_validation_on_connect() {
  if (!_ota_validation_active || _ota_validated) return;

  uint32_t elapsed = millis() - _ota_validation_start_ms;

  if (elapsed > OTA_VALIDATION_WINDOW_MS) {
    Serial.println("[OTA] Validation window expired without MQTT connect");
    Serial.println("[OTA] Rollback will occur on next reboot");
    _ota_validation_active = false;
    return;
  }

  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    _ota_validated         = true;
    _ota_validation_active = false;
    storage_saveOtaPending(0);

    // BUG FIX: this was previously never called on the GSM path (nor
    // anywhere else), so NVS kept reporting the OLD version string
    // even after a successful OTA — the running firmware was
    // genuinely newer, but "[Storage] FW version:" at next boot
    // still showed the pre-update value. Save it now, at the exact
    // moment the new image is confirmed valid and committed.
    storage_saveFwVersion(FW_VERSION);

    Serial.println("[OTA] ========================================");
    Serial.println("[OTA] Firmware validated — OTA committed");
    Serial.print("[OTA] Elapsed since boot: ");
    Serial.print(elapsed / 1000);
    Serial.println("s");
    Serial.println("[OTA] ========================================");
  } else {
    Serial.print("[OTA] mark_valid failed: ");
    Serial.println(esp_err_to_name(err));
  }
}

// ============================================================
//  ──────────────────── WIFI PATH ────────────────────────────
//  COMPLETELY UNCHANGED from the SIM800L version.
// ============================================================

typedef enum {
  WIFI_DISCONNECTED,
  WIFI_CONNECTING,
  WIFI_CONNECTED
} WifiState;

static WifiState _wifi_state         = WIFI_DISCONNECTED;
static uint32_t  _wifi_attempt_start = 0;
static uint32_t  _last_wifi_retry    = 0;

static WiFiClientSecure _wifi_client;
static PubSubClient     _mqtt(_wifi_client);
static uint32_t         _mqtt_backoff_ms   = MQTT_RECONNECT_BASE_MS;
static uint32_t         _last_mqtt_attempt = 0;

static bool _wifi_mqtt_connect_once() {
  Serial.print("[Connect/WiFi] MQTT connecting — client: ");
  Serial.println(_mqtt_client_id);

  _wifi_client.setTimeout(5);

  if (_mqtt.connect(_mqtt_client_id, MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("[Connect/WiFi] MQTT connected");
    _mqtt.subscribe(_topic_settings);
    _mqtt.subscribe(OTA_TOPIC);
    _mqtt_backoff_ms = MQTT_RECONNECT_BASE_MS;
    led_setMode(LED_SOLID);

    _ota_check_validation_on_connect();

    return true;
  }

  Serial.print("[Connect/WiFi] MQTT failed, state: ");
  Serial.println(_mqtt.state());
  _mqtt_backoff_ms = min(_mqtt_backoff_ms * 2, MQTT_RECONNECT_MAX_MS);
  Serial.print("[Connect/WiFi] Next retry in ");
  Serial.print(_mqtt_backoff_ms / 1000);
  Serial.println("s");
  led_setMode(LED_SLOW);
  return false;
}

static void _wifi_start_connect() {
  delay(500);

  String ssid     = storage_loadWifiSSID();
  String password = storage_loadWifiPassword();

  if (ssid.length() == 0) {
    ssid     = WIFI_SSID_DEFAULT;
    password = WIFI_PASSWORD_DEFAULT;
    Serial.println("[Connect/WiFi] No NVS credentials — using hardcoded defaults");
  }

  Serial.print("[Connect/WiFi] Connecting to: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid.c_str(), password.c_str());

  _wifi_state         = WIFI_CONNECTING;
  _wifi_attempt_start = millis();
  led_setMode(LED_SLOW);
}

static void _wifi_loop() {
  switch (_wifi_state) {

    case WIFI_DISCONNECTED: {
      uint32_t now = millis();
      if (now - _last_wifi_retry >= WIFI_RETRY_INTERVAL_MS) {
        _last_wifi_retry = now;
        _wifi_start_connect();
      }
      break;
    }

    case WIFI_CONNECTING: {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[Connect/WiFi] Connected. IP: ");
        Serial.println(WiFi.localIP());
        _wifi_state = WIFI_CONNECTED;

        _wifi_client.setCACert(_mqtt_ca_cert);
        _mqtt.setServer(MQTT_BROKER, MQTT_PORT);
        _mqtt.setCallback(_mqtt_callback);
        _mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
        _mqtt.setBufferSize(MQTT_BUFFER_SIZE);

        _sync_ntp(NTP_SERVER_WIFI);
        _wifi_mqtt_connect_once();

      } else if (millis() - _wifi_attempt_start >= WIFI_CONNECT_TIMEOUT_MS) {
        Serial.println("[Connect/WiFi] Timeout — will retry");
        WiFi.disconnect(true);
        _wifi_state      = WIFI_DISCONNECTED;
        _last_wifi_retry = millis();
        led_setMode(LED_SLOW);
      }
      break;
    }

    case WIFI_CONNECTED: {
      if (!WiFi.isConnected()) {
        Serial.println("[Connect/WiFi] Dropped — retrying");
        _wifi_state      = WIFI_DISCONNECTED;
        _last_wifi_retry = 0;
        led_setMode(LED_SLOW);
        return;
      }

      if (!_mqtt.connected()) {
        uint32_t now = millis();
        if (now - _last_mqtt_attempt >= _mqtt_backoff_ms) {
          _last_mqtt_attempt = now;
          _wifi_mqtt_connect_once();
        }
        return;
      }

      if (_ntp_synced && millis() - _last_ntp_sync >= NTP_SYNC_INTERVAL_MS) {
        _sync_ntp(NTP_SERVER_WIFI);
      }

      _mqtt.loop();
      break;
    }
  }
}

static bool _wifi_publish(PowerData data) {
  if (!_mqtt.connected()) return false;

  StaticJsonDocument<512> doc;
  doc["device_id"]           = _device_id;
  doc["fw_version"]          = FW_VERSION;
  doc["timestamp_iso"]       = _get_iso_timestamp();
  doc["timestamp_ms"]        = (uint32_t)millis();
  doc["conn_mode"]           = "wifi";
  doc["mains_present"]       = data.mains_present;
  doc["voltage_V"]           = serialized(String(data.voltage_V, 1));
  doc["frequency_Hz"]        = serialized(String(data.frequency_Hz, 1));
  doc["rms_current_A"]       = serialized(String(data.rms_current_A, 3));
  doc["power_W"]             = serialized(String(data.power_W, 1));
  doc["power_factor"]        = serialized(String(data.power_factor, 2));
  doc["apparent_power_VA"]   = serialized(String(data.apparent_power_VA, 1));
  doc["total_energy_kWh"]    = serialized(String(data.total_energy_kWh, 4));
  doc["units_purchased_kWh"] = serialized(String(data.units_purchased_kWh, 3));
  doc["units_remaining"]     = serialized(String(data.units_remaining, 3));
  doc["low_units_alert"]     = data.low_units_alert;
  doc["critical_alert"]      = data.critical_units_alert;
  doc["mains_on_duration_s"] = data.mains_on_duration_s;

  String payload;
  serializeJson(doc, payload);

  Serial.print("[Connect/WiFi] Publishing: ");
  Serial.println(payload);

  bool ok = _mqtt.publish(_topic_readings, payload.c_str(), true);
  Serial.println(ok ? "[Connect/WiFi] Published OK" : "[Connect/WiFi] Publish failed");
  return ok;
}

static bool _wifi_publishAlert(bool low_units, bool critical_units) {
  if (!_mqtt.connected()) return false;

  StaticJsonDocument<128> doc;
  doc["device_id"]      = _device_id;
  doc["low_units"]      = low_units;
  doc["critical_units"] = critical_units;
  doc["timestamp_iso"]  = _get_iso_timestamp();

  String payload;
  serializeJson(doc, payload);
  return _mqtt.publish(_topic_alerts, payload.c_str(), true);
}

// ============================================================
//  ──────────────────── GSM PATH — A7670E ────────────────────
//
//  Replaces: TinyGsmClient + SSLClient(BearSSL) wrapping it.
//  With:     TinyGsmClientSecure — the A7670E's OWN native TLS
//            socket (+CCH AT commands under the hood, driven by
//            the lewisxhe/TinyGSM-fork library). PubSubClient
//            sits on top exactly as before — bench-confirmed
//            publish AND subscribe/callback both work correctly
//            against the real production broker.
//
//  REQUIRES: lewisxhe/TinyGSM-fork installed in place of mainline
//  TinyGSM (mainline's SIM7600 profile has TinyGsmClientSecure
//  commented out / unimplemented — confirmed during bench testing).
//  https://github.com/lewisxhe/TinyGSM-fork
// ============================================================

static HardwareSerial     SerialGSM(2);
static TinyGsm             _modem(SerialGSM);
static TinyGsmClientSecure _gsm_ssl_client(_modem, 0);   // mux 0 — A76xx allows mux 0 or 1 only
static PubSubClient        _gsm_mqtt(_gsm_ssl_client);

static bool     _gsm_ready        = false;

// ------------------------------------------------------------
//  GSM TIME — reads time from the cellular network directly via
//  the modem's own AT+CCLK, instead of configTime()/NTP-over-UDP.
//
//  WHY: configTime()/getLocalTime() are built around the ESP32's
//  own WiFi/lwIP network stack. They have no path to reach the
//  A7670E's cellular data connection at all — there is no "modem
//  aware" NTP in the ESP32 core. Confirmed by bench testing:
//  configTime() fails 100% of the time over GSM regardless of
//  which NTP server is configured (tried time.google.com AND
//  pool.ntp.org — both fail identically), while the exact same
//  function works fine over WiFi. This is a mechanism problem,
//  not a server problem.
//
//  Uses TinyGSM's built-in modem.getNetworkTime(), which sends
//  AT+CCLK? and parses the network-provided time/date/timezone.
//  This depends on the carrier actually populating network time
//  (MTN — untested at the time this was written; some carriers
//  disable this at the network level, per community reports —
//  if it never succeeds, that is the likely reason, not a bug
//  here).
//
//  DEFENSIVE HANDLING (known TinyGSM issues: getNetworkTime can
//  return a crash-inducing malformed string on some modem
//  responses, and separately a known "returns year 1939 / epoch"
//  bug on rare occasions — both reported against other SIMCom
//  modules on TinyGSM's issue tracker, not yet seen on our
//  hardware but defended against regardless):
//    - Return value is checked before use.
//    - Year is sanity-checked (2020-2099) before being trusted;
//      obviously-wrong values (1939, 1970, factory-default etc.)
//      are rejected rather than applied.
//    - Pushes the modem's time into the SAME system clock that
//      getLocalTime()/_get_iso_timestamp() already read on the
//      WiFi path, via settimeofday() — so _get_iso_timestamp()
//      itself needs ZERO changes, it keeps working identically
//      regardless of which path (WiFi-NTP or GSM-network-time)
//      set the clock.
// ------------------------------------------------------------
static bool _gsm_sync_time_from_network() {
  Serial.println("[Connect/GSM] Requesting network time (AT+CCLK)...");

  int   year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  float timezone = 0;
  bool  ok = false;

  // The modem's clock is often populated by an asynchronous network
  // notification that can land a few seconds AFTER the data connection
  // comes up — not necessarily at the exact instant GPRS/LTE connects.
  // Querying too early can return a stale/default value even though
  // the modem itself is working fine. Give it a moment, and retry a
  // couple of times before giving up.
  for (int attempt = 0; attempt < 3 && !ok; attempt++) {
    delay(2000);
    ok = _modem.getNetworkTime(&year, &month, &day,
                                &hour, &minute, &second, &timezone);
    if (ok && (year < 2020 || year > 2099)) {
      Serial.print("[Connect/GSM] Network time looks invalid (year ");
      Serial.print(year);
      Serial.println(") — retrying");
      ok = false;
    }
  }

  if (!ok) {
    Serial.println("[Connect/GSM] Network time not available — will retry later");
    return false;
  }

  // Bench-confirmed: AT+CCLK on this network/module already reports
  // LOCAL (WAT) time directly in its date/time fields — verified by
  // comparing the raw modem response against real wall-clock time.
  // So we use those fields exactly as given, with NO offset math.
  // (The `timezone` out-param is intentionally unused here — applying
  // it on top would double-apply an offset that's already baked in.)
  struct tm timeinfo = {};
  timeinfo.tm_year = year - 1900;
  timeinfo.tm_mon  = month - 1;
  timeinfo.tm_mday = day;
  timeinfo.tm_hour = hour;
  timeinfo.tm_min  = minute;
  timeinfo.tm_sec  = second;

  time_t local_epoch = mktime(&timeinfo);
  struct timeval tv = { .tv_sec = local_epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  _ntp_synced    = true;
  _last_ntp_sync = millis();

  struct tm verify;
  getLocalTime(&verify);
  Serial.print("[Connect/GSM] Network time synced: ");
  Serial.println(&verify, "%Y-%m-%dT%H:%M:%S");
  return true;
}
static uint32_t _gsm_mqtt_backoff = MQTT_RECONNECT_BASE_MS;
static uint32_t _gsm_last_attempt = 0;

// ------------------------------------------------------------
//  RAW AT HELPERS — for AT command families TinyGSM does not
//  wrap (HTTP*, CCERTDOWN). These talk directly to SerialGSM,
//  the same UART TinyGSM itself uses. Safe to use between
//  TinyGSM operations as long as nothing else is mid-command at
//  the same time (true here: GSM OTA runs synchronously inside
//  connectivity_loop(), and _gsm_mqtt.loop() is not called during
//  that window — see connectivity_loop()).
//
//  *** THESE HELPERS AND EVERYTHING BUILT ON THEM ARE NEW,
//  UNTESTED-AS-FIRMWARE CODE. *** The underlying AT command
//  SEQUENCE was verified by hand over Tera Term (HTTPINIT,
//  HTTPPARA, SSL config, HTTPACTION all confirmed working,
//  including a real ~1.24MB firmware download completing in
//  10-15s with correct size reported and clean offset reads).
//  What has NOT been tested is this exact C++ parsing logic
//  running as firmware — treat first flash as a debug session,
//  not a proven feature, the same way the MQTT migration needed
//  a few iterations before it was solid.
// ------------------------------------------------------------
static void _gsm_at_flush_input() {
  while (SerialGSM.available()) SerialGSM.read();
}

// Sends a command, waits up to timeout_ms for either `expect` or
// "ERROR" to appear in the response. Returns true/false accordingly.
// out_response, if given, receives everything read (for parsing).
static bool _gsm_at_cmd(const String& cmd, const char* expect,
                        uint32_t timeout_ms, String* out_response = nullptr) {
  _gsm_at_flush_input();
  SerialGSM.println(cmd);

  String resp;
  resp.reserve(256);
  uint32_t start = millis();
  bool found = false;

  while (millis() - start < timeout_ms) {
    while (SerialGSM.available()) {
      char c = (char)SerialGSM.read();
      resp += c;
      if (resp.indexOf(expect) >= 0) { found = true; break; }
      if (resp.indexOf("ERROR") >= 0) { found = false; goto done; }
    }
    if (found) break;
    esp_task_wdt_reset();
  }
done:
  if (out_response) *out_response = resp;
  return found;
}

// ------------------------------------------------------------
//  Uploads isrgrootx1.pem to the module's filesystem via
//  AT+CCERTDOWN, if it is not already present. Makes GSM_CACERT
//  self-provisioning — no manual per-unit step required.
//
//  The cert content below is IDENTICAL to what was manually
//  uploaded via Tera Term and bench-verified working (both for
//  MQTT cert-verified connect and HTTPS OTA download). Byte count
//  must match EXACTLY what AT+CCERTDOWN is told to expect —
//  verified 1939 bytes with Unix (LF) line endings.
// ------------------------------------------------------------
static const char _isrg_root_x1_pem[] PROGMEM =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n";

#define GSM_CACERT_PEM_BYTES 1939   // MUST match the exact byte count of the PEM above (LF endings)

static bool _gsm_provision_cert_if_missing() {
  String resp;

  // Check whether the cert is already present — AT+CCERTLIST
  if (!_gsm_at_cmd("AT+CCERTLIST", "OK", 5000, &resp)) {
    Serial.println("[Connect/GSM] WARNING — AT+CCERTLIST failed, cannot check cert state");
    return false;
  }
  if (resp.indexOf(GSM_CACERT_FILENAME) >= 0) {
    return true;   // already present, nothing to do
  }

  Serial.println("[Connect/GSM] Cert not found on module — uploading (first-boot provisioning)...");

  // Kick off the download: AT+CCERTDOWN="<filename>",<size> then
  // wait for the ">" prompt before sending raw bytes.
  String cmd = String("AT+CCERTDOWN=\"") + GSM_CACERT_FILENAME + "\"," + String(GSM_CACERT_PEM_BYTES);
  if (!_gsm_at_cmd(cmd, ">", 5000, &resp)) {
    Serial.println("[Connect/GSM] Cert upload FAILED — no '>' prompt from AT+CCERTDOWN");
    return false;
  }

  // Send the PEM bytes directly (from PROGMEM), no line-ending
  // translation — must match GSM_CACERT_PEM_BYTES exactly.
  _gsm_at_flush_input();
  size_t len = strlen_P(_isrg_root_x1_pem);
  for (size_t i = 0; i < len; i++) {
    SerialGSM.write((uint8_t)pgm_read_byte(&_isrg_root_x1_pem[i]));
  }

  // Wait for the final OK confirming the upload completed
  String uploadResp;
  uint32_t start = millis();
  bool ok = false;
  while (millis() - start < 10000) {
    while (SerialGSM.available()) {
      uploadResp += (char)SerialGSM.read();
      if (uploadResp.indexOf("OK") >= 0) { ok = true; break; }
      if (uploadResp.indexOf("ERROR") >= 0) { ok = false; goto uploadDone; }
    }
    if (ok) break;
    esp_task_wdt_reset();
  }
uploadDone:
  if (!ok) {
    Serial.println("[Connect/GSM] Cert upload FAILED — no OK after sending bytes");
    return false;
  }

  Serial.println("[Connect/GSM] Cert uploaded successfully — provisioning complete for this unit");
  return true;
}

// ------------------------------------------------------------
//  Re-applies the CA certificate reference on the SSL socket.
//
//  BENCH-DISCOVERED QUIRK: the A7670E's SSL context config
//  (cacert filename + authmode) is RESET on every module
//  power-cycle/reboot, even though the certificate FILE itself
//  persists in the module's filesystem (survives reboot fine).
//  So this must be called before every connect attempt, not
//  just once at boot — calling it when already-correct is a
//  harmless no-op AT command.
//
//  setCertificate() takes a FILENAME (not PEM content) — the
//  cert must be uploaded to the module via AT+CCERTDOWN first.
//  This is now self-provisioning (see _gsm_provision_cert_if_missing
//  above): a fresh module with no cert loaded will have it
//  uploaded automatically on first connect attempt, no manual
//  per-unit step required.
// ------------------------------------------------------------
static bool _gsm_ensure_cert_loaded() {
  if (!_gsm_provision_cert_if_missing()) {
    Serial.println("[Connect/GSM] WARNING — could not confirm/provision cert on this module.");
    return false;
  }
  bool ok = _gsm_ssl_client.setCertificate(GSM_CACERT_FILENAME);
  if (!ok) {
    Serial.println("[Connect/GSM] WARNING — setCertificate failed even though "
                    "the cert appears to be on the module. Falling back to "
                    "unverified TLS would be a downgrade — NOT done automatically.");
  }
  return ok;
}

// ------------------------------------------------------------
//  GSM OTA DOWNLOAD — via the A7670E's native HTTP(S) AT commands.
//
//  *** STATUS: the AT command SEQUENCE below was verified by hand
//  over Tera Term against the REAL production OTA endpoint — a
//  genuine ~1.24MB firmware binary downloaded successfully via
//  HTTPS with full certificate verification in ~10-15 seconds,
//  and offset-based AT+HTTPREAD calls returned correct, intact
//  data (verified by recognising real firmware strings in the
//  returned bytes). What has NOT been tested is this exact
//  chunked-read-and-write C++ loop running as actual firmware —
//  treat the first flash of this as a debugging session. ***
//
//  Sequence: HTTPTERM (clean slate) -> HTTPINIT -> set URL ->
//  bind SSL context 0 -> ensure cert loaded/verified (reuses the
//  same cert as MQTT) -> HTTPACTION=0 (GET, async — module returns
//  OK immediately, then an unsolicited "+HTTPACTION: 0,<code>,<size>"
//  arrives once the full response has been fetched INTO THE MODEM
//  — the download itself already happened by this point) -> loop
//  AT+HTTPREAD=<chunk> to pull the data back over UART in
//  1024-byte pieces (the module's max block size — it reads
//  sequentially from its own internal pointer, offsets are NOT
//  used), writing each piece to the OTA partition as it arrives.
//
//  IMPORTANT: unlike the WiFi/esp_https_ota() path, the "download"
//  and the "transfer to ESP32" are two separate phases here — by
//  the time HTTPACTION reports success, the file is already fully
//  fetched by the modem; the read loop below is pulling it out of
//  the modem's own buffer, not still downloading it. This means a
//  slow cellular link mostly affects the HTTPACTION wait, not the
//  read loop.
// ------------------------------------------------------------
#define GSM_OTA_HTTPACTION_TIMEOUT_MS   120000UL   // generous — bench saw ~10-15s for 1.24MB
// BENCH-DISCOVERED: the A7670E caps each +HTTPREAD DATA BLOCK at
// 1024 bytes. Requesting more does NOT fail — the module returns
// multiple consecutive "+HTTPREAD: 1024" blocks then "+HTTPREAD: 0".
//
// So we use two sizes:
//   GSM_OTA_BLOCK_BYTES   — the module's hard per-block cap (1024).
//                           Our read buffer must be at least this.
//   GSM_OTA_REQUEST_BYTES — how much we ask for per AT command. Larger
//                           = fewer AT round-trips = much faster, since
//                           the per-command overhead dominates at 1KB.
//                           8192 => ~8 blocks per command, ~160 commands
//                           for a 1.3MB file instead of ~1280.
#define GSM_OTA_BLOCK_BYTES             1024
#define GSM_OTA_REQUEST_BYTES           8192

// ------------------------------------------------------------
//  POWERCALC CO-EXISTENCE DURING OTA
//
//  Without yielding, the OTA loop saturates the CPU (tight UART
//  reads + back-to-back flash writes) and starves the PZEM task on
//  Core 0 for the whole download — PZEM readings stop entirely for
//  the duration. That is SAFE (the connectivity_ota_in_progress
//  guard in power_calc.cpp prevents the starved NaN reads from
//  being misread as genuine mains loss, and energy totals are
//  untouched) but it means ~2 minutes of no metering.
//
//  These two knobs trade download speed for metering continuity:
//    GSM_OTA_YIELD_EVERY_N_BLOCKS — how often to pause (1 = after
//        every 1024-byte block; higher = less often = faster).
//    GSM_OTA_YIELD_MS            — how long to pause. Must be long
//        enough for the PZEM task to actually COMPLETE a read
//        transaction on UART1, not just get scheduled. PZEM samples
//        roughly once a second, so the goal is to give it a real
//        window at least that often.
//
//  Baseline for reference: with NO yielding, a 1,309,920-byte
//  download completed in 117s at ~11,195 B/s (~97% of the 115200
//  baud UART ceiling — i.e. essentially no overhead left to remove).
//  Any yielding will slow that down; the values below are chosen
//  deliberately GENEROUS to prioritise PZEM continuity, since an
//  OTA taking a few minutes is acceptable for a device that
//  updates rarely.
//
//  TUNING: if PZEM lines still do not appear during the download,
//  RAISE GSM_OTA_YIELD_MS. If the download is slower than you want
//  and PZEM is healthy, RAISE GSM_OTA_YIELD_EVERY_N_BLOCKS first
//  (pause less often) before shortening the pause itself.
//  To revert to maximum speed entirely, set
//  GSM_OTA_YIELD_EVERY_N_BLOCKS to 0 (disables yielding).
// ------------------------------------------------------------
#define GSM_OTA_YIELD_EVERY_N_BLOCKS    2      // pause every 2 blocks (~2KB)
#define GSM_OTA_YIELD_MS                40     // pause duration (ms)
#define GSM_OTA_READ_TIMEOUT_MS         15000UL

static bool _gsm_ota_do_update(const char* url, const char* version) {
  Serial.println("[OTA/GSM] ========================================");
  Serial.print("[OTA/GSM] Starting native HTTP(S) download — version: ");
  Serial.println(version);
  Serial.print("[OTA/GSM] URL: ");
  Serial.println(url);

  connectivity_ota_in_progress = true;
  led_setMode(LED_FAST);

  String resp;

  // ---- Clean slate ----
  _gsm_at_cmd("AT+HTTPTERM", "OK", 3000, &resp);   // ignore result — errors if nothing was active, harmless

  if (!_gsm_at_cmd("AT+HTTPINIT", "OK", 5000, &resp)) {
    Serial.println("[OTA/GSM] HTTPINIT failed — aborting");
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  String urlCmd = String("AT+HTTPPARA=\"URL\",\"") + url + "\"";
  if (!_gsm_at_cmd(urlCmd, "OK", 5000, &resp)) {
    Serial.println("[OTA/GSM] Setting URL failed — aborting");
    _gsm_at_cmd("AT+HTTPTERM", "OK", 3000);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  _gsm_at_cmd("AT+HTTPPARA=\"SSLCFG\",0", "OK", 3000, &resp);

  if (!_gsm_ensure_cert_loaded()) {
    Serial.println("[OTA/GSM] Cert not confirmed — aborting rather than "
                    "downloading firmware without verified TLS");
    _gsm_at_cmd("AT+HTTPTERM", "OK", 3000);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  esp_task_wdt_reset();

  // ---- Fire the GET. This blocks (module-side) until the full
  // response is fetched into the modem, then reports the result. ----
  Serial.println("[OTA/GSM] Requesting firmware (AT+HTTPACTION=0)...");
  if (!_gsm_at_cmd("AT+HTTPACTION=0", "OK", 5000, &resp)) {
    Serial.println("[OTA/GSM] HTTPACTION command rejected — aborting");
    _gsm_at_cmd("AT+HTTPTERM", "OK", 3000);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  // Now wait for the unsolicited "+HTTPACTION: 0,<code>,<size>" line.
  String actionResp;
  uint32_t start = millis();
  bool gotAction = false;
  while (millis() - start < GSM_OTA_HTTPACTION_TIMEOUT_MS) {
    while (SerialGSM.available()) {
      actionResp += (char)SerialGSM.read();
      if (actionResp.indexOf("+HTTPACTION:") >= 0 &&
          actionResp.indexOf('\n', actionResp.indexOf("+HTTPACTION:")) >= 0) {
        gotAction = true;
        break;
      }
    }
    if (gotAction) break;
    esp_task_wdt_reset();
    delay(100);
  }

  if (!gotAction) {
    Serial.println("[OTA/GSM] Timed out waiting for HTTPACTION result — aborting");
    _gsm_at_cmd("AT+HTTPTERM", "OK", 3000);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  // Parse: +HTTPACTION: <method>,<status>,<size>
  int idx = actionResp.indexOf("+HTTPACTION:");
  int c1 = actionResp.indexOf(',', idx);
  int c2 = actionResp.indexOf(',', c1 + 1);
  int c3 = actionResp.indexOf('\n', c2 + 1);
  int http_status   = actionResp.substring(c1 + 1, c2).toInt();
  int32_t file_size  = actionResp.substring(c2 + 1, c3 > 0 ? c3 : actionResp.length()).toInt();

  Serial.print("[OTA/GSM] HTTP status: ");
  Serial.print(http_status);
  Serial.print(" | size: ");
  Serial.print(file_size);
  Serial.println(" bytes");

  if (http_status != 200 || file_size <= 0) {
    Serial.println("[OTA/GSM] Bad status or size — aborting");
    _gsm_at_cmd("AT+HTTPTERM", "OK", 3000);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  // ---- Begin the OTA partition write ----
  const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
  if (!update_partition) {
    Serial.println("[OTA/GSM] No OTA partition available — aborting");
    _gsm_at_cmd("AT+HTTPTERM", "OK", 3000);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  esp_ota_handle_t ota_handle = 0;
  esp_err_t err = esp_ota_begin(update_partition, file_size, &ota_handle);
  if (err != ESP_OK) {
    Serial.print("[OTA/GSM] esp_ota_begin failed: ");
    Serial.println(esp_err_to_name(err));
    _gsm_at_cmd("AT+HTTPTERM", "OK", 3000);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  // ---- Chunked read-and-write loop ----
  int32_t offset = 0;
  bool write_error = false;
  int chunk_count = 0;
  uint32_t dl_start_ms = millis();

  // Let the module settle after HTTPACTION completes, and clear any
  // trailing bytes left in the UART buffer from the +HTTPACTION
  // response. Without this the first read's header parse can latch
  // onto stale data. (Bench note: reading by hand in a terminal
  // always worked because of the natural human delay between
  // commands — the firmware fires the read milliseconds later.)
  delay(500);
  _gsm_at_flush_input();

  static uint8_t chunk_buf[GSM_OTA_BLOCK_BYTES];

  while (offset < file_size) {
    int32_t want = min((int32_t)GSM_OTA_REQUEST_BYTES, file_size - offset);

    // BENCH-DISCOVERED (two things):
    //  1. The module IGNORES the offset parameter and reads
    //     sequentially from its own internal pointer — issuing
    //     AT+HTTPREAD=0,N twice returns DIFFERENT (consecutive)
    //     data, not the same block twice. So no offset is passed.
    //  2. The module caps each DATA BLOCK at 1024 bytes. Asking for
    //     more does not fail — it returns MULTIPLE consecutive
    //     "+HTTPREAD: 1024" blocks, then a final "+HTTPREAD: 0".
    //     We exploit that here: request a large amount per AT
    //     command and consume all the blocks it produces, which
    //     cuts the number of AT round-trips (and their overhead)
    //     by ~8x versus one command per 1024 bytes.
    String readCmd = String("AT+HTTPREAD=") + want;
    _gsm_at_flush_input();
    SerialGSM.println(readCmd);

    int32_t got_this_request = 0;
    uint32_t req_start = millis();

    // Consume blocks until the module emits "+HTTPREAD: 0" (end of
    // this request's data) or we've taken everything we asked for.
    while (got_this_request < want && !write_error) {

      // ---- Parse one block header: "+HTTPREAD: <n>" ----
      // IMPORTANT: find the newline that comes AFTER the marker, not
      // just any newline — echoed command text and stale bytes also
      // contain newlines, and matching those yields a garbage count.
      String header;
      uint32_t hstart = millis();
      bool gotHeader = false;
      int  hIdx = -1, nEnd = -1;

      while (millis() - hstart < GSM_OTA_READ_TIMEOUT_MS) {
        while (SerialGSM.available()) {
          header += (char)SerialGSM.read();
          hIdx = header.indexOf("+HTTPREAD:");
          if (hIdx >= 0) {
            nEnd = header.indexOf('\n', hIdx);
            if (nEnd > hIdx) { gotHeader = true; break; }
          }
          if (header.indexOf("ERROR") >= 0) {
            Serial.print("[OTA/GSM] Module returned ERROR at offset ");
            Serial.println(offset);
            Serial.print("[OTA/GSM] Raw: [");
            Serial.print(header);
            Serial.println("]");
            write_error = true;
            break;
          }
        }
        if (gotHeader || write_error) break;
        esp_task_wdt_reset();
      }

      if (write_error) break;
      if (!gotHeader) {
        Serial.print("[OTA/GSM] Timed out waiting for block header at offset ");
        Serial.println(offset);
        Serial.print("[OTA/GSM] Raw so far: [");
        Serial.print(header);
        Serial.println("]");
        write_error = true;
        break;
      }

      int32_t block_len = header.substring(hIdx + 10, nEnd).toInt();

      // "+HTTPREAD: 0" = end of data for this request. Normal.
      if (block_len == 0) break;

      if (block_len < 0 || block_len > GSM_OTA_BLOCK_BYTES) {
        Serial.print("[OTA/GSM] Implausible block length ");
        Serial.print(block_len);
        Serial.print(" at offset ");
        Serial.println(offset);
        write_error = true;
        break;
      }

      // ---- Read exactly block_len raw bytes ----
      // Binary-safe: raw byte buffer, not String (firmware bytes can
      // be any value including 0x00).
      int32_t got = 0;
      uint32_t rstart = millis();
      while (got < block_len && millis() - rstart < GSM_OTA_READ_TIMEOUT_MS) {
        int avail = SerialGSM.available();
        if (avail > 0) {
          int toRead = min((int32_t)avail, block_len - got);
          got += SerialGSM.readBytes(chunk_buf + got, toRead);
          rstart = millis();      // reset idle timer on progress
        } else {
          esp_task_wdt_reset();
        }
      }

      if (got != block_len) {
        Serial.print("[OTA/GSM] Short read at offset ");
        Serial.print(offset);
        Serial.print(" — expected ");
        Serial.print(block_len);
        Serial.print(", got ");
        Serial.println(got);
        write_error = true;
        break;
      }

      err = esp_ota_write(ota_handle, chunk_buf, got);
      if (err != ESP_OK) {
        Serial.print("[OTA/GSM] esp_ota_write failed: ");
        Serial.println(esp_err_to_name(err));
        write_error = true;
        break;
      }

      offset            += got;
      got_this_request  += got;
      chunk_count++;
      esp_task_wdt_reset();

      // Give Core 0 (PowerCalc/PZEM) a real window to run. See the
      // GSM_OTA_YIELD_* comment block above for tuning guidance.
      // vTaskDelay (not delay()) genuinely yields to the scheduler
      // rather than busy-waiting, which is what PZEM needs.
      #if GSM_OTA_YIELD_EVERY_N_BLOCKS > 0
      if (chunk_count % GSM_OTA_YIELD_EVERY_N_BLOCKS == 0) {
        vTaskDelay(pdMS_TO_TICKS(GSM_OTA_YIELD_MS));
      }
      #endif
    }

    if (write_error) break;

    // Progress + throughput instrumentation. Logged per AT request
    // (not per 1024-byte block) so it stays readable while still
    // giving real timing data to diagnose any future slowness.
    uint32_t elapsed_s = (millis() - dl_start_ms) / 1000;
    Serial.print("[OTA/GSM] Progress: ");
    Serial.print((int)((offset * 100L) / file_size));
    Serial.print("% (");
    Serial.print(offset);
    Serial.print("/");
    Serial.print(file_size);
    Serial.print(" bytes) | this req: ");
    Serial.print(millis() - req_start);
    Serial.print("ms | elapsed: ");
    Serial.print(elapsed_s);
    Serial.print("s | avg: ");
    int32_t rate = elapsed_s > 0 ? (offset / (int32_t)elapsed_s) : 0;
    Serial.print(rate);
    Serial.print(" B/s | est total: ");
    Serial.print(rate > 0 ? (file_size / rate) : 0);
    Serial.println("s");

    // NOTE: no separate "trailer drain" here. The block loop above
    // already consumes the terminating "+HTTPREAD: 0", and the next
    // iteration flushes anything left before issuing its command.
    // (The previous implementation waited on a 5s timeout after every
    // 1024-byte chunk, which alone accounted for ~1.8 HOURS across
    // the full file — that was the cause of the observed slowness.)
  }

  _gsm_at_cmd("AT+HTTPTERM", "OK", 3000);

  if (write_error || offset != file_size) {
    Serial.println("[OTA/GSM] Download incomplete/failed — aborting OTA");
    esp_ota_abort(ota_handle);
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  err = esp_ota_end(ota_handle);
  if (err != ESP_OK) {
    Serial.print("[OTA/GSM] esp_ota_end/validation failed: ");
    Serial.println(esp_err_to_name(err));
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    Serial.print("[OTA/GSM] esp_ota_set_boot_partition failed: ");
    Serial.println(esp_err_to_name(err));
    connectivity_ota_in_progress = false;
    led_setMode(LED_SLOW);
    return false;
  }

  storage_saveOtaPending(1);

  Serial.println("[OTA/GSM] ========================================");
  Serial.println("[OTA/GSM] Flash complete — rebooting into new firmware");
  Serial.print("[OTA/GSM] New version: ");
  Serial.println(version);
  Serial.println("[OTA/GSM] Must connect to MQTT within validation window to confirm.");
  Serial.println("[OTA/GSM] ========================================");

  delay(1000);
  esp_restart();
  return true;   // unreachable, keeps compiler happy
}

static bool _gsm_uart_init() {

  Serial.println("[Connect/GSM] Starting UART2...");

  // BENCH-DISCOVERED: ESP32 HardwareSerial's default RX ring buffer
  // (256 bytes) is too small once GSM OTA introduces deliberate
  // scheduler yields (see GSM_OTA_YIELD_MS in the OTA section) —
  // the modem keeps transmitting at ~11.5 KB/s during every pause,
  // and once the default buffer fills, the UART driver SILENTLY
  // DROPS bytes. That byte loss desyncs the OTA read loop's block
  // accounting, which can then misinterpret bytes from INSIDE the
  // firmware payload (which literally contains our own AT-command
  // string constants, e.g. "+HTTPREAD:") as a protocol header.
  // Must be called BEFORE begin().
  SerialGSM.setRxBufferSize(4096);
  SerialGSM.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);

  Serial.println("[Connect/GSM] Waiting for modem boot...");
  for (int i = 0; i < 5; i++) {
    esp_task_wdt_reset();
    delay(1000);
  }

  Serial.println("[Connect/GSM] Testing AT...");
  for (int attempt = 0; attempt < 10; attempt++) {
    esp_task_wdt_reset();
    if (_modem.testAT(1000)) {
      Serial.println("[Connect/GSM] Modem responded — UART OK");
      return true;
    }
  }

  Serial.println("[Connect/GSM] No response — check 5V rail, GND bridge, GPIO16/17, baud (115200)");
  return false;
}

static bool _gsm_gprs_init() {
  Serial.println("[Connect/GSM] Initialising modem via TinyGSM...");
  esp_task_wdt_reset();

  if (!_modem.restart()) {
    Serial.println("[Connect/GSM] Modem restart failed");
    return false;
  }
  Serial.println("[Connect/GSM] Modem restarted");
  esp_task_wdt_reset();

  String info = _modem.getModemInfo();
  Serial.print("[Connect/GSM] Modem info: ");
  Serial.println(info);

  Serial.print("[Connect/GSM] Waiting for network registration...");
  if (!_modem.waitForNetwork(GSM_CONNECT_TIMEOUT_MS)) {
    Serial.println(" FAILED");
    return false;
  }
  Serial.println(" registered");
  esp_task_wdt_reset();

  Serial.print("[Connect/GSM] Signal quality: ");
  Serial.println(_modem.getSignalQuality());

  // ---- LTE APN: auto-negotiated from SIM, no manual APN/cycling
  // needed (was required on 2G/SIM800L, confirmed unnecessary on
  // A7670E LTE during bench testing — APN blank in config.h). ----
  Serial.println("[Connect/GSM] Connecting GPRS/LTE (auto APN)...");
  esp_task_wdt_reset();

  if (!_modem.gprsConnect(GSM_APN, GSM_APN_USER, GSM_APN_PASS)) {
    Serial.println("[Connect/GSM] GPRS/LTE connect FAILED");
    return false;
  }

  Serial.print("[Connect/GSM] Connected. IP: ");
  Serial.println(_modem.localIP());
  return true;
}

static bool _gsm_mqtt_connect_once() {
  Serial.print("[Connect/GSM] MQTT connecting — broker: ");
  Serial.print(GSM_MQTT_BROKER);
  Serial.print(":");
  Serial.println(GSM_MQTT_PORT);

  // Ensure the SSL context has the CA cert re-applied — see
  // _gsm_ensure_cert_loaded() comment: this resets on every
  // module reboot and must be reasserted before each connect.
  if (!_gsm_ensure_cert_loaded()) {
    Serial.println("[Connect/GSM] Aborting connect attempt — cert not confirmed loaded");
    _gsm_mqtt_backoff = min(_gsm_mqtt_backoff * 2, MQTT_RECONNECT_MAX_MS);
    return false;
  }

  _gsm_mqtt.setServer(GSM_MQTT_BROKER, GSM_MQTT_PORT);
  _gsm_mqtt.setCallback(_mqtt_callback);
  _gsm_mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  _gsm_mqtt.setBufferSize(MQTT_BUFFER_SIZE);

  esp_task_wdt_reset();

  if (_gsm_mqtt.connect(_mqtt_client_id, MQTT_USERNAME, MQTT_PASSWORD)) {
    Serial.println("[Connect/GSM] MQTT connected");
    _gsm_mqtt.subscribe(_topic_settings);
    _gsm_mqtt.subscribe(OTA_TOPIC);
    _gsm_mqtt_backoff = MQTT_RECONNECT_BASE_MS;
    led_setMode(LED_SOLID);
    _ota_check_validation_on_connect();
    return true;
  }

  Serial.print("[Connect/GSM] MQTT failed, state: ");
  Serial.println(_gsm_mqtt.state());
  _gsm_mqtt_backoff = min(_gsm_mqtt_backoff * 2, MQTT_RECONNECT_MAX_MS);
  Serial.print("[Connect/GSM] Next retry in ");
  Serial.print(_gsm_mqtt_backoff / 1000);
  Serial.println("s");
  led_setMode(LED_SLOW);
  return false;
}

static void _gsm_init() {
  Serial.println("[Connect/GSM] Initialising A7670E...");
  _gsm_ready = false;

  if (!_gsm_uart_init()) {
    Serial.println("[Connect/GSM] UART init failed — will keep retrying in loop");
    return;
  }

  if (!_gsm_gprs_init()) {
    Serial.println("[Connect/GSM] GPRS/LTE init failed — will keep retrying in loop");
    return;
  }

  if (!_ntp_synced) {
    _gsm_sync_time_from_network();   // configTime()/NTP-over-UDP does not work over
                                      // the modem — see _gsm_sync_time_from_network()
                                      // comment for why. Failure here is non-fatal;
                                      // it just retries via the same call in _gsm_loop().
  }

  _gsm_ready = true;
  _gsm_mqtt_connect_once();
}

// ------------------------------------------------------------
//  BENCH-DISCOVERED QUIRK: the A7670E's internal MQTT/SSL
//  service can wedge into a state where it stops responding
//  correctly and a simple reconnect no longer works. The bench
//  recovery was a full modem reset. We surface that here as a
//  ceiling-triggered hard reset: if backoff has maxed out
//  MODEM_RESET_AFTER_N_FAILS times in a row, do a full
//  _gsm_init() from scratch (which calls modem.restart()) rather
//  than just retrying the MQTT connect. This is a defensive
//  addition beyond what SIM800L needed — untested for the exact
//  wedge condition in this integrated loop context (only
//  confirmed manually over AT commands on the bench). Flagging
//  for extra attention during integration testing.
// ------------------------------------------------------------
static uint8_t _gsm_consecutive_backoff_ceilings = 0;
#define GSM_MODEM_RESET_AFTER_N_CEILINGS  3

static void _gsm_loop() {
  if (!_gsm_ready) {
    uint32_t now = millis();
    if (now - _gsm_last_attempt >= _gsm_mqtt_backoff) {
      _gsm_last_attempt = now;
      Serial.println("[Connect/GSM] Retrying GSM init...");
      _gsm_init();
    }
    return;
  }

  if (!_gsm_mqtt.connected()) {
    uint32_t now = millis();
    if (now - _gsm_last_attempt >= _gsm_mqtt_backoff) {
      _gsm_last_attempt = now;

      esp_task_wdt_reset();

      if (!_modem.isGprsConnected()) {
        Serial.println("[Connect/GSM] GPRS/LTE dropped — reconnecting...");
        if (!_modem.gprsConnect(GSM_APN, GSM_APN_USER, GSM_APN_PASS)) {
          Serial.println("[Connect/GSM] GPRS/LTE reconnect failed");
          _gsm_mqtt_backoff = min(_gsm_mqtt_backoff * 2, MQTT_RECONNECT_MAX_MS);
          return;
        }
        Serial.println("[Connect/GSM] GPRS/LTE reconnected");
      }

      if (!_gsm_mqtt_connect_once()) {
        if (_gsm_mqtt_backoff >= MQTT_RECONNECT_MAX_MS) {
          _gsm_consecutive_backoff_ceilings++;
          Serial.print("[Connect/GSM] Backoff ceiling reached (");
          Serial.print(_gsm_consecutive_backoff_ceilings);
          Serial.println(" in a row)");

          if (_gsm_consecutive_backoff_ceilings >= GSM_MODEM_RESET_AFTER_N_CEILINGS) {
            Serial.println("[Connect/GSM] Too many ceilings — forcing full modem re-init");
            _gsm_consecutive_backoff_ceilings = 0;
            _gsm_ready = false;   // drops through to full _gsm_init() next loop
          }
          _gsm_mqtt_backoff = MQTT_RECONNECT_BASE_MS;
        }
      } else {
        _gsm_consecutive_backoff_ceilings = 0;
      }
    }
    return;
  }

  if (_ntp_synced && millis() - _last_ntp_sync >= NTP_SYNC_INTERVAL_MS) {
    _gsm_sync_time_from_network();
  }

  _gsm_mqtt.loop();
}

static bool _gsm_publish(PowerData data) {
  if (!_gsm_mqtt.connected()) return false;

  StaticJsonDocument<512> doc;
  doc["device_id"]           = _device_id;
  doc["fw_version"]          = FW_VERSION;
  doc["timestamp_iso"]       = _get_iso_timestamp();
  doc["timestamp_ms"]        = (uint32_t)millis();
  doc["conn_mode"]           = "gsm";
  doc["mains_present"]       = data.mains_present;
  doc["voltage_V"]           = serialized(String(data.voltage_V, 1));
  doc["frequency_Hz"]        = serialized(String(data.frequency_Hz, 1));
  doc["rms_current_A"]       = serialized(String(data.rms_current_A, 3));
  doc["power_W"]             = serialized(String(data.power_W, 1));
  doc["power_factor"]        = serialized(String(data.power_factor, 2));
  doc["apparent_power_VA"]   = serialized(String(data.apparent_power_VA, 1));
  doc["total_energy_kWh"]    = serialized(String(data.total_energy_kWh, 4));
  doc["units_purchased_kWh"] = serialized(String(data.units_purchased_kWh, 3));
  doc["units_remaining"]     = serialized(String(data.units_remaining, 3));
  doc["low_units_alert"]     = data.low_units_alert;
  doc["critical_alert"]      = data.critical_units_alert;
  doc["mains_on_duration_s"] = data.mains_on_duration_s;

  String payload;
  serializeJson(doc, payload);

  Serial.print("[Connect/GSM] Publishing: ");
  Serial.println(payload);

  bool ok = _gsm_mqtt.publish(_topic_readings, payload.c_str(), true);
  Serial.println(ok ? "[Connect/GSM] Published OK" : "[Connect/GSM] Publish failed");
  return ok;
}

static bool _gsm_publishAlert(bool low_units, bool critical_units) {
  if (!_gsm_mqtt.connected()) return false;

  StaticJsonDocument<128> doc;
  doc["device_id"]      = _device_id;
  doc["low_units"]      = low_units;
  doc["critical_units"] = critical_units;
  doc["timestamp_iso"]  = _get_iso_timestamp();

  String payload;
  serializeJson(doc, payload);
  return _gsm_mqtt.publish(_topic_alerts, payload.c_str(), true);
}

// ============================================================
//  PUBLIC API — unchanged
// ============================================================

void connectivity_init(const char* device_id) {
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms     = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_reconfigure(&wdt_config);

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      Serial.println("[OTA] Rollback: partition pending verification — good");
    } else if (ota_state == ESP_OTA_IMG_VALID) {
      Serial.println("[OTA] Rollback: partition already valid");
    } else {
      Serial.println("[OTA] WARNING — rollback may not be enabled (sdkconfig)");
    }
  }

  strncpy(_device_id, device_id, sizeof(_device_id) - 1);

  snprintf(_topic_readings, sizeof(_topic_readings),
           "smartmeter/%s/readings", _device_id);
  snprintf(_topic_settings, sizeof(_topic_settings),
           "smartmeter/%s/settings", _device_id);
  snprintf(_topic_alerts, sizeof(_topic_alerts),
           "smartmeter/%s/alerts", _device_id);
  snprintf(_mqtt_client_id, sizeof(_mqtt_client_id),
           "SmartMeter_%s", _device_id);

  Serial.print("[Connect] Device ID: ");  Serial.println(_device_id);
  Serial.print("[Connect] Readings:  ");  Serial.println(_topic_readings);
  Serial.print("[Connect] Settings:  ");  Serial.println(_topic_settings);
  Serial.print("[Connect] Alerts:    ");  Serial.println(_topic_alerts);
  Serial.print("[Connect] OTA topic: ");  Serial.println(OTA_TOPIC);

  _conn_mode = storage_loadConnMode();
  Serial.print("[Connect] Mode: ");
  Serial.println(_conn_mode == CONN_MODE_WIFI ? "WiFi" : "GSM");

  if (storage_loadOtaPending() == 1) {
    _ota_validation_active   = true;
    _ota_validation_start_ms = millis();
    _ota_validated           = false;
    Serial.println("[OTA] *** Validation boot detected ***");
    Serial.print("[OTA] Must connect within ");
    Serial.print(OTA_VALIDATION_WINDOW_MS / 60000);
    Serial.println(" minutes to commit firmware");
  }

  memset(&_pending_settings, 0, sizeof(_pending_settings));

  if (_conn_mode == CONN_MODE_WIFI) {
    _wifi_start_connect();
  } else {
    _gsm_init();
  }
}

void connectivity_loop() {

  if (_ota_validation_active && !_ota_validated) {
    if (millis() - _ota_validation_start_ms >= OTA_VALIDATION_WINDOW_MS) {
      Serial.println("[OTA] Validation window expired without MQTT connect");
      Serial.println("[OTA] Forcing rollback now...");
      _ota_validation_active = false;
      delay(1000);
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }

  if (_ota_requested) {
    _ota_requested = false;
    _ota_do_update(_ota_url, _ota_version);
    Serial.println("[OTA] Download failed — resuming normal operation");
  }

  if (_conn_mode == CONN_MODE_WIFI) {
    _wifi_loop();
  } else {
    _gsm_loop();
  }
}

bool connectivity_publish(PowerData data) {
  if (connectivity_ota_in_progress) return false;

  if (_conn_mode == CONN_MODE_WIFI) return _wifi_publish(data);
  return _gsm_publish(data);
}

bool connectivity_publishAlert(bool low_units, bool critical_units) {
  if (connectivity_ota_in_progress) return false;

  if (_conn_mode == CONN_MODE_WIFI) return _wifi_publishAlert(low_units, critical_units);
  return _gsm_publishAlert(low_units, critical_units);
}

bool connectivity_isConnected() {
  if (_conn_mode == CONN_MODE_WIFI) return _mqtt.connected();
  return _gsm_mqtt.connected();
}

void connectivity_getSettings(ConnectivitySettings& settings) {
  settings        = _pending_settings;
  _settings_ready = false;
  memset(&_pending_settings, 0, sizeof(_pending_settings));
}

void connectivity_setMode(uint8_t mode) {
  if (mode == _conn_mode) return;
  _conn_mode = mode;
  storage_saveConnMode(mode);
  Serial.print("[Connect] Mode changed to: ");
  Serial.println(mode == CONN_MODE_WIFI ? "WiFi" : "GSM");

  if (mode == CONN_MODE_GSM) {
    delay(2000);
    _gsm_ready        = false;
    _gsm_mqtt_backoff = MQTT_RECONNECT_BASE_MS;
    _gsm_last_attempt = 0;
    _gsm_init();
  } else {
    if (_gsm_mqtt.connected()) _gsm_mqtt.disconnect();
    _wifi_state      = WIFI_DISCONNECTED;
    _last_wifi_retry = 0;
  }
}