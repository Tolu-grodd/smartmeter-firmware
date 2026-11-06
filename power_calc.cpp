#include "power_calc.h"
#include "connectivity.h"   // for connectivity_ota_in_progress (see NaN branch below)
#include <PZEM004Tv30.h>

static PowerData _data;
static float     _voltage        = DEFAULT_VOLTAGE_V;
static float     _units_budget   = 0.0f;
static uint32_t  _last_energy_ms = 0;
static bool      _alert_sent     = false;
static uint32_t  _mains_on_start_ms = 0;
static bool      _mains_was_present = false;

// ============================================================
//  PHASE 2 — PZEM state (used only when POWER_SOURCE == PHASE_PZEM)
// ============================================================
#if POWER_SOURCE == PHASE_PZEM
static PZEM004Tv30 _pzem(Serial1, PZEM_RX_PIN, PZEM_TX_PIN);
static int  _pzem_warmup_count  = 0;
static bool _pzem_recovering    = false;
static uint32_t _last_print_ms  = 0;
static uint32_t _oor_start_ms = 0;
static uint32_t _oor_count    = 0;
#endif

// ============================================================
//  INTERNAL — energy accumulation
//  Called by both ADC and PZEM paths after filling power fields
// ============================================================
static void _accumulate_energy() {
  uint32_t now        = millis();
  float    elapsed_hr = (float)(now - _last_energy_ms) / 3600000.0f;
  float    delta_Wh   = _data.power_W * elapsed_hr;

  _data.energy_Wh        += delta_Wh;
  _data.total_energy_kWh += (delta_Wh / 1000.0f);
  _last_energy_ms         = now;

  // Units remaining
  if (_units_budget > 0.0f) {
    _data.units_remaining = _units_budget - _data.total_energy_kWh;
    if (_data.units_remaining < 0.0f) _data.units_remaining = 0.0f;
  }

  // Alert flags
  _data.low_units_alert      = (_data.units_remaining <= LOW_UNITS_THRESHOLD &&
                                _units_budget > 0.0f);
  _data.critical_units_alert = (_data.units_remaining <= CRITICAL_UNITS_THRESHOLD &&
                                _units_budget > 0.0f);
}

// ============================================================
//  PHASE 1 — ADC / CT clamp implementation
// ============================================================
#if POWER_SOURCE == PHASE_ADC

static int _adc_midpoint = ADC_MIDPOINT;

static void _adc_init() {
  analogSetPinAttenuation(ADC_PIN, ADC_11db);

  // Auto-calibrate midpoint — sample with no load
  long sum = 0;
  for (int i = 0; i < 200; i++) {
    sum += analogRead(ADC_PIN);
    delayMicroseconds(500);
  }
  _adc_midpoint = (int)(sum / 200);

  pinMode(ADC_PIN, INPUT);

  Serial.print("[PowerCalc] ADC midpoint calibrated: ");
  Serial.println(_adc_midpoint);
}

static void _adc_sample() {
  uint32_t start  = millis();
  float    sum_sq = 0.0f;
  uint32_t count  = 0;

  while (millis() - start < SAMPLE_DURATION_MS) {
    int   raw      = analogRead(ADC_PIN);
    float centered = (float)(raw - _adc_midpoint);
    float sample_V = centered * (ADC_VREF / (float)ADC_RESOLUTION);

    sum_sq += sample_V * sample_V;
    count++;
    delayMicroseconds(100);
  }

  if (count == 0) return;

  float rms_V = sqrt(sum_sq / (float)count);
  _data.rms_current_A = rms_V * CT_CALIBRATION;

  if (_data.rms_current_A < NOISE_FLOOR_AMPS) {
    _data.rms_current_A = 0.0f;
  }

  // Phase 1 — assumed/fixed values
  _data.voltage_V        = _voltage;
  _data.power_W          = _data.rms_current_A * _voltage;
  _data.power_factor     = 1.0f;
  _data.frequency_Hz     = 50.0f;
  _data.apparent_power_VA = _data.power_W;
  _data.mains_present    = true;

  _accumulate_energy();

  Serial.print("[PowerCalc] ADC | Current: ");
  Serial.print(_data.rms_current_A, 3);
  Serial.print("A | Power: ");
  Serial.print(_data.power_W, 1);
  Serial.print("W | Total: ");
  Serial.print(_data.total_energy_kWh, 4);
  Serial.print("kWh | Remaining: ");
  Serial.println(_data.units_remaining, 3);
}

#endif // PHASE_ADC

// ============================================================
//  PHASE 2 — PZEM-004T implementation
//
//  Hardware wiring:
//    PZEM TX → ESP32 GPIO18 (PZEM_RX_PIN)
//    PZEM RX → ESP32 GPIO19 (PZEM_TX_PIN)
//    PZEM GND → ESP32 GND (shared ground — mandatory)
//    PZEM powers itself from its own mains screw terminals
//    Do NOT feed 5V from ESP32 to PZEM — it draws from mains directly
//
//  Library: PZEM004Tv30 by Mandriota
//    Install via Arduino Library Manager before compiling
//
//  Notes:
//    - PZEM object declared at top of file (Serial1, RX, TX)
//    - Serial1.begin() is handled inside the PZEM library constructor
//      No separate Serial1.begin() call needed
//    - PZEM internal energy register is deliberately ignored —
//      it resets on power loss. We accumulate in NVS instead.
//    - NaN on any reading = mains absent (PZEM timeout)
//    - Warmup discard: first PZEM_WARMUP_READINGS after recovery
//      are skipped to let PZEM measurements stabilise
// ============================================================
#if POWER_SOURCE == PHASE_PZEM

static void _pzem_init() {
  // Serial1 is initialised by the PZEM004Tv30 constructor above.
  // Nothing else to do here — PZEM is ready as soon as setup() runs.
  Serial.println("[PowerCalc] PZEM-004T initialised on Serial1");
  Serial.print("[PowerCalc] RX pin: GPIO");
  Serial.print(PZEM_RX_PIN);
  Serial.print(" | TX pin: GPIO");
  Serial.println(PZEM_TX_PIN);
}

static void _pzem_sample() {
  // Read all values from PZEM
  float voltage   = _pzem.voltage();
  float current   = _pzem.current();
  float power     = _pzem.power();
  float pf        = _pzem.pf();
  float frequency = _pzem.frequency();
  // Note: _pzem.energy() deliberately not used — see header comment

  // Check for NaN on voltage — primary indicator of mains absent or PZEM timeout.
  // If voltage is NaN the other readings are also unreliable.
  bool reading_valid = !isnan(voltage) && !isnan(current) &&
                       !isnan(power)   && !isnan(pf)      &&
                       !isnan(frequency);

  if (!reading_valid) {

    // ------------------------------------------------------------
    //  OTA GUARD — added after bench-observed false "mains lost".
    //
    //  During a GSM OTA download the connectivity task blocks Core 1
    //  for an extended period (~10-15s) while the modem fetches the
    //  firmware over HTTP(S), and again while the chunked AT+HTTPREAD
    //  loop pulls ~1.3MB back over UART. That CPU/UART pressure can
    //  starve this PZEM read enough that it times out and returns
    //  NaN — which previously got misread as genuine mains loss,
    //  triggering a spurious "*** POWER FAIL — NVS saved ***" and
    //  zeroing the live readings mid-OTA.
    //
    //  Energy totals were never corrupted by this (the NaN branch
    //  deliberately skips _accumulate_energy()), but the false
    //  power-fail is misleading in logs and would publish bogus
    //  mains_present=false to the backend.
    //
    //  So: during OTA, treat a NaN as "reading unavailable, try
    //  again shortly" rather than "mains has gone". We deliberately
    //  do NOT touch _data here — the last known-good values stay
    //  in place — and we do NOT reset _last_energy_ms, because the
    //  gap is short and real consumption during it is negligible.
    //
    //  NOTE: this means a genuine mains loss occurring *during* an
    //  OTA download will not be detected until the OTA finishes.
    //  Accepted trade-off: OTA is infrequent and short-lived, and
    //  the supercap/NVS-save path still protects stored totals.
    // ------------------------------------------------------------
    if (connectivity_ota_in_progress) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      return;
    }

    // Mains absent or PZEM not responding
    if (_data.mains_present) {
      // Transition: was present, now lost
      Serial.println("[PowerCalc] PZEM — mains lost (NaN reading)");
      _pzem_recovering    = true;
      _pzem_warmup_count  = 0;
      _mains_was_present = false;
    }

    _data.mains_present     = false;
    _data.power_W           = 0.0f;
    _data.rms_current_A     = 0.0f;
    _data.voltage_V         = 0.0f;
    _data.power_factor      = 0.0f;
    _data.frequency_Hz      = 0.0f;
    _data.apparent_power_VA = 0.0f;
    _data.mains_on_duration_s = 0;
    // Do NOT call _accumulate_energy() — no power being consumed

    // Still update timing so we don't accumulate a phantom energy spike
    // when mains returns and elapsed_hr is huge
    _last_energy_ms = millis();
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }

  // Reading is valid — check if we are in warmup after recovery
  if (_pzem_recovering) {
    _pzem_warmup_count++;
    Serial.print("[PowerCalc] PZEM warmup reading ");
    Serial.print(_pzem_warmup_count);
    Serial.print("/");
    Serial.println(PZEM_WARMUP_READINGS);

    if (_pzem_warmup_count >= PZEM_WARMUP_READINGS) {
      _pzem_recovering = false;
      Serial.println("[PowerCalc] PZEM warmup complete — trusting readings");
    } else {
      // Reset timing here too so elapsed_hr stays small when we start accumulating
      _last_energy_ms = millis();
      return;
    }
  }

 // Sanity check — reject physically impossible readings
  if (voltage < 80.0f || voltage > 260.0f ||
      current < 0.0f  || current > 100.0f ||
      power   < 0.0f  || power   > 23000.0f ||
      pf      < 0.0f  || pf      > 1.0f ||
      frequency < 45.0f || frequency > 65.0f) {

    if (_oor_count == 0) _oor_start_ms = millis();
    _oor_count++;

    Serial.print("[PowerCalc] PZEM — out of range (count: ");
    Serial.print(_oor_count);
    Serial.print(", duration: ");
    Serial.print(millis() - _oor_start_ms);
    Serial.println("ms)");

    _last_energy_ms = millis();
    vTaskDelay(pdMS_TO_TICKS(1000));
    return;
  }

  // Valid reading — if we just recovered from out of range, report it
  if (_oor_count > 0) {
    Serial.print("[PowerCalc] PZEM — out of range cleared after ");
    Serial.print(_oor_count);
    Serial.print(" readings over ");
    Serial.print(millis() - _oor_start_ms);
    Serial.println("ms");
    _oor_count = 0;
  }

  // Valid reading, warmup done — fill _data and accumulate
  _data.mains_present     = true;
    // Start or continue mains-on timer
  if (!_mains_was_present) {
    _mains_on_start_ms  = millis();
    _mains_was_present  = true;
  }
  _data.mains_on_duration_s = (millis() - _mains_on_start_ms) / 1000;
  _data.voltage_V         = voltage;
  _data.rms_current_A     = current;
  _data.power_W           = power;
  _data.power_factor      = pf;
  _data.frequency_Hz      = frequency;

  // Apparent power = V × I (PZEM does not report this directly)
  _data.apparent_power_VA = (pf > 0.0f) ? (power / pf) : 0.0f;

  _accumulate_energy();

  if (millis() - _last_print_ms >= 2000) {
    _last_print_ms = millis();
    Serial.print("[PowerCalc] PZEM | V: ");
    Serial.print(_data.voltage_V, 1);
    Serial.print("V | I: ");
    Serial.print(_data.rms_current_A, 3);
    Serial.print("A | P: ");
    Serial.print(_data.power_W, 1);
    Serial.print("W | PF: ");
    Serial.print(_data.power_factor, 2);
    Serial.print(" | Hz: ");
    Serial.print(_data.frequency_Hz, 1);
    Serial.print(" | Total: ");
    Serial.print(_data.total_energy_kWh, 4);
    Serial.print("kWh | Remaining: ");
    Serial.println(_data.units_remaining, 3);
  }

  vTaskDelay(pdMS_TO_TICKS(1000));
}

#endif // PHASE_PZEM
// ============================================================
//  PUBLIC API
// ============================================================
void powerCalc_init(float voltage) {
  _voltage = voltage;

  memset(&_data, 0, sizeof(_data));
  _data.voltage_V         = voltage;
  _data.power_factor      = 1.0f;
  _data.frequency_Hz      = 50.0f;
  _data.apparent_power_VA = 0.0f;
  _data.mains_present     = true;

  _last_energy_ms = millis();

#if POWER_SOURCE == PHASE_ADC
  _adc_init();
  Serial.println("[PowerCalc] Phase 1 — ADC/CT clamp active");
#elif POWER_SOURCE == PHASE_PZEM
  _pzem_init();
  Serial.println("[PowerCalc] Phase 2 — PZEM-004T active");
#endif
}

void powerCalc_sample() {
#if POWER_SOURCE == PHASE_ADC
  _adc_sample();
#elif POWER_SOURCE == PHASE_PZEM
  _pzem_sample();
#endif
}

PowerData powerCalc_getData()    { return _data; }

bool powerCalc_isMainsPresent()  { return _data.mains_present; }

void powerCalc_setVoltage(float v) {
  _voltage        = v;
  _data.voltage_V = v;
  Serial.print("[PowerCalc] Voltage updated: ");
  Serial.println(v);
}

void powerCalc_setUnitsBudget(float units_kWh) {
  _units_budget             = units_kWh;
  _data.units_purchased_kWh = units_kWh;
  _alert_sent               = false;
  Serial.print("[PowerCalc] Units budget set: ");
  Serial.println(units_kWh);
}

bool powerCalc_checkAlert() {
  if (_data.low_units_alert && !_alert_sent) {
    _alert_sent = true;
    return true;
  }
  return false;
}

void powerCalc_resetSession() {
  _data.energy_Wh = 0.0f;
  _last_energy_ms = millis();
}

void powerCalc_setTotalEnergy(float total_kWh) {
  _data.total_energy_kWh = total_kWh;
  Serial.print("[PowerCalc] Total energy restored: ");
  Serial.println(total_kWh, 4);
}