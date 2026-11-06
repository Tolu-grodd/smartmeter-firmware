# Smart Energy Meter Firmware

Firmware for a prepaid single-phase smart energy meter built on the ESP32. It measures
electricity use, deducts it from a prepaid balance that survives power cuts, reports
readings over MQTT with TLS (Wi-Fi or 4G), accepts remote settings and top-ups, and updates
itself over the air with automatic rollback.

## Features

- **Metering**: reads power and energy from a PZEM-004T module over UART, or from a CT clamp
  through the ADC. The source is a compile-time choice (`POWER_SOURCE` in `config.h`).
- **Prepaid balance**: units remaining are tracked and saved to flash (NVS), with low and
  critical thresholds. Saves happen every minute, and every 10 seconds during the first
  minute after boot, so a power cut loses very little.
- **Power-fail handling**: optional supercapacitor power-fail detection by interrupt, plus
  detection of the PZEM going silent, so the balance is saved before the meter dies.
- **Connectivity**: Wi-Fi, or 4G Cat-1 through an A7670E modem with its native TLS. The mode
  is stored in NVS, and the meter retries cellular periodically when running on Wi-Fi.
- **MQTT over TLS** (port 8883) with broker credentials. Readings are published every
  2 minutes; settings and alerts have their own topics, with automatic reconnect and
  exponential backoff.
- **Time**: NTP over Wi-Fi, and network time from the modem (`AT+CCLK`) over cellular.
- **Provisioning**: a captive-portal setup page served by the meter itself (an open access
  point named after the device ID, with the page baked into the firmware). BLE
  provisioning is also implemented, and is disabled by default to save flash.
- **Reset button**: hold 3 seconds to re-provision (energy totals are kept), 10 seconds for a
  full factory reset.
- **Dual-core FreeRTOS design**: energy measurement runs on core 0, connectivity on core 1,
  with task watchdogs on both.

## Over-the-air updates

```
backend ──MQTT──►  { "ota_version": "1.1.0", "ota_url": "https://host/fw/smartmeter_1.1.0.bin" }
                       │
        device compares ota_version with FW_VERSION; downloads only if newer
                       │
        esp_https_ota downloads and flashes the other partition, then reboots
                       │
   ┌───────────────────┴────────────────────┐
   MQTT reconnects within the validation     Window expires without a connection
   window (10 minutes)                       → rollback to the previous firmware
   → update is marked valid
```

- The download URL arrives in the trigger message, so it is never hard-coded in the firmware.
- Rollback is enabled in the bootloader (`sdkconfig.defaults`) and driven by the firmware.
- The partition layout (`partitions_backup.csv`) is a 4 MB board with two OTA slots.

## MQTT topics

| Topic | Direction | Purpose |
| --- | --- | --- |
| `smartmeter/<device_id>/readings` | meter → broker | Periodic JSON readings: voltage, current, power, apparent power, power factor, frequency, total energy, units purchased and remaining, mains status, firmware version, timestamp |
| `smartmeter/<device_id>/settings` | broker → meter | Remote commands: top-up (`topup_kWh`), set balance (`set_units_kWh`), voltage, phone number for alerts, connectivity mode, Wi-Fi credentials |
| `smartmeter/<device_id>/alerts` | meter → broker | Low-balance and critical-balance alerts (retained) |
| `smartmeter/ota` | broker → meter | Firmware update trigger (above) |

The device ID is derived from the Wi-Fi MAC address (for example `SM-D3BEFC`).

## Hardware

| Part | Notes |
| --- | --- |
| ESP32-WROOM-32 (4 MB) | Main MCU |
| PZEM-004T v3 | UART1, RX 18 / TX 19, 9600 baud (default power source) |
| CT clamp | Alternative power source on ADC pin 34 |
| A7670E 4G modem | UART2, RX 17 / TX 16, 115200 baud |
| Status LED | Pin 4 |
| Reset button | Pin 32 |
| Power-fail sense | Pin 35 (supercapacitor circuit) |

## Build

1. Install the ESP32 board package in the Arduino IDE (or arduino-cli) and these libraries:
   ArduinoJson, PubSubClient, PZEM004Tv30 and a TinyGSM version with A76xx support
   (the code notes reference the lewisxhe fork).
2. Copy `secrets.h.example` to `secrets.h` and fill in your MQTT login (and, if needed, the
   Wi-Fi fallback and APN login). `secrets.h` is git-ignored.
3. Set `MQTT_BROKER` in `config.h` to your broker. Bump `FW_VERSION` before building a binary
   you intend to ship as an OTA update.
4. Choose a partition layout that fits two OTA slots, such as the one in
   `partitions_backup.csv`, and keep `sdkconfig.defaults` beside the sketch so rollback is
   enabled.

## Known limitations

- **OTA over cellular is not implemented.** `esp_https_ota` works on the ESP32's own Wi-Fi
  network stack and cannot see the modem's data connection. The two ways forward are PPPoS
  or a chunked download over the modem's own HTTP commands. Wi-Fi OTA works.
- BLE provisioning is switched off by default (`BLE_PROVISION_ENABLED`); the captive portal
  is the supported path.
- On the 4 MB development board the binary uses most of each OTA slot, so there is little
  headroom for new features.
- The Wi-Fi fallback credentials are only used when flash is empty and provisioning times
  out; they are not meant for production devices.

## Files

| File | Role |
| --- | --- |
| `SmartMeter_OTA.ino` | Entry point, tasks and the main loop |
| `config.h` | All compile-time settings, pins and timing |
| `connectivity.cpp/.h` | Wi-Fi and cellular, MQTT, OTA, time sync |
| `power_calc.cpp/.h` | PZEM and ADC measurement and energy accounting |
| `storage.cpp/.h` | Persistent settings and balance in NVS |
| `ap_provision.cpp/.h` | Captive-portal setup page |
| `ble_provision.cpp/.h` | BLE provisioning |
| `led.cpp/.h` | Status LED patterns |
| `certificates.h` | ISRG Root X1 trust anchor (public CA certificate) |
