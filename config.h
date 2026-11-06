#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Credentials live in secrets.h (git-ignored). Copy secrets.h.example to secrets.h and fill it in.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing secrets.h: copy secrets.h.example to secrets.h and fill in your values."
#endif

// ============================================================
//  FIRMWARE VERSION
//  Increment this before building a new OTA binary.
//  Device compares incoming ota_version against this value
//  and skips download if version matches or is older.
// ============================================================
#define FW_VERSION                "1.0.0"    

// ============================================================
//  POWER SOURCE SELECTOR
// ============================================================
#define PHASE_ADC                 0
#define PHASE_PZEM                1
#define POWER_SOURCE              PHASE_PZEM      // ← Change back to PHASE_PZEM or PHASE_ADC

// ============================================================
//  CONNECTIVITY MODE DEFAULT
//  Saved in NVS after first set. This is compile-time default only.
//  0 = WiFi
//  1 = GSM (A7670E)
// ============================================================
#define CONN_MODE_WIFI            0
#define CONN_MODE_GSM             1
#define DEFAULT_CONN_MODE         CONN_MODE_WIFI

// ============================================================
//  WIFI — fallback defaults for development
//  Real credentials provisioned via BLE/AP and saved in NVS
//  These only activate if NVS is empty AND BLE times out
// ============================================================
// WIFI_SSID_DEFAULT: defined in secrets.h
// WIFI_PASSWORD_DEFAULT: defined in secrets.h
#define WIFI_CONNECT_TIMEOUT_MS   20000UL
#define WIFI_RETRY_INTERVAL_MS    10000UL

// ============================================================
//  GSM — A7670E (4G Cat-1, replaces SIM800L — see MIGRATION NOTES
//  at bottom of this file for what changed and why)
//  UART2 — does not conflict with PZEM (UART1) or debug (UART0)
//
//  APN: left blank. On LTE the network auto-assigns the correct
//  APN from the SIM profile — MTN/Airtel/9mobile all confirmed
//  auto-configuring during bench testing. The old APN-cycling list
//  (internet/glo/etisalat/airtelgprs.com) was a SIM800L/2G-era
//  requirement and is no longer used — see connectivity.cpp.
// ============================================================
#define GSM_RX_PIN                17
#define GSM_TX_PIN                16
#define GSM_BAUD                  115200        // was 9600 on SIM800L
#define GSM_APN                   ""            // blank = auto (LTE assigns from SIM)
// GSM_APN_USER: defined in secrets.h
// GSM_APN_PASS: defined in secrets.h
#define GSM_CONNECT_TIMEOUT_MS    30000UL
#define GSM_RETRY_INTERVAL_MS     60000UL
#define GSM_FALLBACK_RETRY_MS     1800000UL     // Retry GSM after 30 min if on WiFi fallback

// Certificate filename as stored on the module's own filesystem via
// AT+CCERTDOWN. Must be pre-loaded once per unit at manufacture/
// provisioning (or by the firmware's self-provisioning check — see
// connectivity.cpp _gsm_ensure_cert_loaded()). NOT a PEM string —
// the A76xx native SSL takes a filename reference, unlike
// WiFiClientSecure::setCACert() which takes PEM content directly.
#define GSM_CACERT_FILENAME       "isrgrootx1.pem"

// ============================================================
//  MQTT
//  Dev:  HiveMQ Cloud — TLS port 8883
//  Prod: Own VPS Mosquitto — TLS port 8883 (WiFi: mbedTLS,
//        GSM: A7670E native TLS). Plain 1883 NOT used — the
//        A7670E does reliable native TLS, so the SIM800L-era
//        plain-MQTT workaround plan was not needed.
// ============================================================
#define MQTT_BROKER               "mqtt.example.com"
#define MQTT_PORT                 8883             
// MQTT_USERNAME: defined in secrets.h
// MQTT_PASSWORD: defined in secrets.h
#define MQTT_KEEPALIVE_S          60
#define MQTT_BUFFER_SIZE          512
#define MQTT_RECONNECT_BASE_MS    5000UL
#define MQTT_RECONNECT_MAX_MS     300000UL      // Backoff ceiling — 5 minutes

// ============================================================
//  BLE PROVISIONING
// ============================================================
#define BLE_DEVICE_PREFIX         "SM-"
#define BLE_TIMEOUT_MS            300000UL      // 5 minutes then fall back to defaults
#define BLE_SERVICE_UUID          "12345678-1234-1234-1234-123456789abc"
#define BLE_CHAR_PROVISION_UUID   "abcd1234-ab12-ab12-ab12-abcdef123456"
#define BLE_CHAR_STATUS_UUID      "abcd1235-ab12-ab12-ab12-abcdef123456"

// ============================================================
//  AP PROVISIONING
//  Captive portal — no SPIFFS, HTML baked into firmware
//  AP SSID = device ID (e.g. "SM-D3BEFC"), open network
//  Runs in parallel with BLE — whichever completes first wins
// ============================================================
#define AP_TIMEOUT_MS             300000UL      // 5 minutes, matches BLE

// ============================================================
//  PZEM-004T — Phase 2
//  UART1 — separate from GSM (UART2) and debug (UART0)
// ============================================================
#define PZEM_RX_PIN               18
#define PZEM_TX_PIN               19
#define PZEM_BAUD                 9600
#define PZEM_TIMEOUT_MS           3000UL
#define PZEM_WARMUP_READINGS      3

// ============================================================
//  ADC — Phase 1 (CT clamp)
//  GPIO34 = ADC1 channel 6, input only, WiFi safe
// ============================================================
#define ADC_PIN                   34
#define ADC_RESOLUTION            4095
#define ADC_VREF                  3.3f
#define ADC_MIDPOINT              2048
#define CT_CALIBRATION            30.0f
#define NOISE_FLOOR_AMPS          0.5f
#define SAMPLE_DURATION_MS        2000UL

// ============================================================
//  VOLTAGE — Phase 1 assumed, Phase 2+ measured by PZEM
// ============================================================
#define DEFAULT_VOLTAGE_V         220.0f

// ============================================================
//  UNITS TRACKING
// ============================================================
#define DEFAULT_UNITS_PURCHASED_KWH  50.0f
#define LOW_UNITS_THRESHOLD          5.0f
#define CRITICAL_UNITS_THRESHOLD     2.0f

// ============================================================
//  TIMING
// ============================================================
#define UPLOAD_INTERVAL_MS        120000UL      // MQTT publish every 2 mins
#define NVS_SAVE_INTERVAL_MS      60000UL       // NVS save every 1 min
#define NTP_SYNC_INTERVAL_MS      3600000UL     // Re-sync NTP every hour
#define BOOT_SAVE_INTERVAL_MS     10000UL       // NVS save every 10s for first 60s
#define BOOT_SAVE_DURATION_MS     60000UL       // Boot vulnerability window

// ============================================================
//  NTP — West Africa Time = UTC+1, no DST
//
//  Two separate server settings, used by two separate code paths:
//    - NTP_SERVER_WIFI: used via configTime()/UDP NTP on the WiFi
//      path only. Proven reliable — do not change without testing,
//      it was previously switched to time.google.com after
//      pool.ntp.org gave problems on WiFi.
//    - NTP_SERVER_GSM: NOT ACTUALLY USED as a server hostname —
//      the GSM path does not use configTime()/NTP at all (bench-
//      confirmed configTime() cannot reach any server over the
//      modem, tried both time.google.com and pool.ntp.org, both
//      failed identically). Instead GSM reads time from the
//      cellular network directly via _gsm_sync_time_from_network()
//      in connectivity.cpp (AT+CCLK through TinyGSM). This define
//      is kept only for reference / in case a future modem-aware
//      NTP approach is added; it is not referenced anywhere yet.
// ============================================================
#define NTP_SERVER_WIFI           "time.google.com"
#define NTP_SERVER_GSM            "pool.ntp.org"     // unused — see comment above
#define NTP_GMT_OFFSET_S          3600
#define NTP_DST_OFFSET_S          0

// ============================================================
//  FACTORY RESET & REPROVISIONING
//  Hold BOOT button (GPIO0) during normal operation:
//    3s  → re-provisioning mode (keeps energy totals, replaces WiFi creds)
//    10s → full factory reset (wipes all NVS)
//  3s grace period after boot prevents false triggers during flashing
// ============================================================
#define FACTORY_RESET_PIN         32
#define REPROVISION_HOLD_MS       3000UL        // Hold 3s → re-provision
#define FACTORY_RESET_HOLD_MS     10000UL       // Hold 10s → factory reset

// ============================================================
//  STATUS LED
// ============================================================
#define LED_PIN                   4

// ============================================================
//  SUPERCAP POWER FAIL — add when hardware arrives
//  Voltage divider on 5V rail → GPIO35
//  ISR fires on falling edge when mains cuts
// ============================================================
#define POWER_FAIL_PIN            35
// #define POWER_FAIL_ENABLED     // Uncomment when supercap circuit for gpio is wired
#define POWER_FAIL_PZEM_ENABLED   // PZEM NaN transition — no extra hardware needed

// ============================================================
//  OTA (Over-The-Air Update)
//
//  Trigger: backend publishes to smartmeter/ota topic:
//    { "ota_version": "1.1.0", "ota_url": "https://your-vps/fw/smartmeter_1.1.0.bin" }
//
//  Device compares ota_version against FW_VERSION. Downloads only if newer.
//
//  *** IMPORTANT — GSM OTA IS NOT YET IMPLEMENTED FOR THE A7670E ***
//  esp_https_ota() (used below) runs on the ESP32's own lwIP/WiFi
//  network stack. It has NO visibility into the A7670E's cellular
//  data connection, which is a completely separate stack reached
//  only via AT commands over UART. So OTA over GSM will NOT work
//  by simply swapping the MQTT transport — this is a SEPARATE,
//  UNSOLVED design problem, tracked separately from the MQTT
//  migration in this file. See handoff doc section "OTA over GSM".
//  Candidate solutions (neither implemented yet):
//    (a) PPPoS — make the A7670E a real ESP-IDF network interface,
//        after which esp_https_ota() works unmodified.
//    (b) Manual chunked download via A7670E native AT+HTTPS / FTP
//        commands, feeding bytes to esp_ota_write() by hand.
//  WiFi OTA is UNAFFECTED and continues to work as-is.
//
//  Rollback:
//    After OTA reboot, device has OTA_VALIDATION_WINDOW_MS to connect to MQTT.
//    If connection succeeds within that window → mark valid, OTA committed.
//    If window expires without connection → rollback on next reboot.
//    Window resets on every reboot (count lives in RAM only, not NVS).
//    Power outage during validation window = fresh window on next boot. Intentional.
// ============================================================
#define OTA_TOPIC                 "smartmeter/ota"
#define OTA_URL_MAX_LEN           256
#define OTA_VERSION_MAX_LEN       16

// Time allowed after OTA reboot to reach MQTT before rollback is permitted.
// Long enough to survive transient network issues (SIM registration, GPRS attach).
// GSM path needs more time — 10 minutes is conservative but safe.
#define OTA_VALIDATION_WINDOW_MS  600000UL      // 10 minutes

// How many ms between watchdog feeds during OTA download.
// esp_https_ota_perform() returns after each chunk (~4KB).
// This is not a sleep — it is called as fast as the transport delivers data.
// Watchdog is fed on every call regardless of chunk timing.
#define OTA_CHUNK_WDT_FEED        1             // feed wdt on every perform() call

// Progress log interval — print download % every N chunks to avoid flooding Serial
#define OTA_PROGRESS_LOG_CHUNKS   50

// ============================================================
//  FREERTOS
// ============================================================
#define CORE_ENERGY               0
#define CORE_CONNECTIVITY         1
#define PRIORITY_ENERGY           3
#define PRIORITY_CONNECT          2
#define STACK_ENERGY              4096
#define STACK_CONNECT             10240         // Increased from 8192 — OTA uses extra stack
#define CORE_SAMPLE_WATCHDOG_MS   3000UL
#define WDT_TIMEOUT_S             300


#define BLE_PROVISION_ENABLED  0   // Set to 1 when BLE provisioning is needed, did this to save space.

// ============================================================
//  MIGRATION NOTES — SIM800L → A7670E (read before touching GSM code)
//
//  WHY: SIM800L's TLS is per-unit unreliable (confirmed by TinyGSM
//  maintainers — varies module to module even on identical firmware)
//  and its software-TLS path (BearSSL/SSLClient) is architecturally
//  unsound over the lossy UART link. Bench-proven: A7670E connects
//  with full cert verification in ~5-11s, first attempt, every time.
//
//  WHAT CHANGED:
//    - GSM_BAUD: 9600 -> 115200
//    - TINY_GSM_MODEM_SIM800 -> TINY_GSM_MODEM_A76XXSSL (connectivity.cpp)
//    - SSLClient(wrapping TinyGsmClient) -> TinyGsmClientSecure (native)
//    - certificates.h (BearSSL binary trust-anchor format) no longer
//      used on the GSM path — the A7670E takes a cert FILENAME
//      (GSM_CACERT_FILENAME), not PEM/binary content. certificates.h
//      can be deleted once WiFi path independently confirmed fine
//      (WiFi still uses its own PEM string, unaffected by this).
//    - APN cycling list removed — LTE auto-negotiates APN from SIM.
//
//  WHAT DID NOT CHANGE:
//    - Broker, port (8883), topics, MQTT credentials — identical.
//    - _mqtt_callback(), OTA trigger parsing, settings parsing —
//      untested change needed; PubSubClient API is unchanged from
//      the app's point of view, callback proven working over the
//      A7670E's native SSL socket during bench testing.
//    - WiFi path — completely untouched.
//
//  KNOWN A7670E QUIRKS (bench-discovered, must be handled in
//  connectivity.cpp reconnect logic):
//    1. SSL context config (cacert, authmode) resets on every module
//       reboot/power-cycle. The cert FILE persists (AT+CCERTLIST
//       still shows it) but the config pointing at it does not.
//       -> Firmware must re-apply setCertificate() on every
//          (re)connect attempt, not just once at boot.
//    2. The module's internal MQTT/SSL service can wedge into a
//       state where neither start nor stop AT commands succeed.
//       Recovery requires a full modem reset (AT+CRESET equivalent
//       via TinyGSM's modem.restart()/modem.init()).
//    3. Native MQTT session (AT+CMQTT layer) drops after a period
//       of inactivity beyond keepalive if the app layer doesn't
//       service it — same general behavior as any MQTT client,
//       but confirm PubSubClient's .loop() is being called
//       frequently enough in _gsm_loop() (it is, in the existing
//       structure — no change needed there, flagging for awareness).
//
//  NOT YET SOLVED — see OTA section above: GSM OTA needs its own
//  implementation (PPP or manual chunked AT download). Do not
//  assume swapping the MQTT transport also fixes OTA-over-GSM.
// ============================================================

#endif