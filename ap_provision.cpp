#include "ap_provision.h"
#include "storage.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

// ============================================================
//  STATE
// ============================================================
static WebServer  _server(80);
static DNSServer  _dns;
static bool       _is_complete = false;
static bool       _timed_out   = false;
static uint32_t   _start_time  = 0;
static char       _ap_ssid[32] = {0};

// ============================================================
//  WIFI SCAN — deduplicated, sorted by signal strength.
//
//  Requires WIFI_AP_STA (not plain WIFI_AP) — the STA radio does
//  the scanning while the AP radio keeps serving the captive
//  portal. Scan is started ASYNCHRONOUSLY (WiFi.scanNetworks(true))
//  so it does not block the DNS/HTTP server loop for the few
//  seconds a scan takes — a blocking scan here would make the
//  phone's captive-portal detector think the page is unresponsive.
//
//  Capped at GSM_SCAN_MAX_NETWORKS entries to keep this simple
//  (plain arrays, no heap/STL) and bound memory use regardless of
//  how many networks are nearby.
// ============================================================
#define WIFI_SCAN_MAX_NETWORKS 20

static bool _scan_kicked_off = false;

static void _wifi_scan_start() {
  WiFi.scanNetworks(true /* async */, false /* show_hidden */);
  _scan_kicked_off = true;
}

// Builds a JSON array of {"ssid":"...","rssi":-45,"secure":true},
// deduplicated (keep strongest RSSI per SSID) and sorted strongest
// first. Returns "" if the scan is still running or hasn't started.
static String _wifi_scan_results_json() {
  int n = WiFi.scanComplete();

  if (n == WIFI_SCAN_RUNNING) return "";          // still scanning
  if (n == WIFI_SCAN_FAILED) {                    // never started / failed
    if (!_scan_kicked_off) _wifi_scan_start();
    return "";
  }

  // n >= 0: results ready.
  struct Net { String ssid; int32_t rssi; bool secure; };
  static Net nets[WIFI_SCAN_MAX_NETWORKS];
  int count = 0;

  for (int i = 0; i < n && count < WIFI_SCAN_MAX_NETWORKS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;             // skip hidden networks

    // Dedupe: same SSID may appear on multiple channels/APs —
    // keep only the strongest signal we've seen for it.
    bool dup = false;
    for (int j = 0; j < count; j++) {
      if (nets[j].ssid == ssid) {
        dup = true;
        if (WiFi.RSSI(i) > nets[j].rssi) {
          nets[j].rssi   = WiFi.RSSI(i);
          nets[j].secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        }
        break;
      }
    }
    if (dup) continue;

    nets[count].ssid   = ssid;
    nets[count].rssi   = WiFi.RSSI(i);
    nets[count].secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    count++;
  }

  // Simple selection sort by RSSI descending — count is small
  // (<= WIFI_SCAN_MAX_NETWORKS), so O(n^2) is fine here.
  for (int i = 0; i < count - 1; i++) {
    int best = i;
    for (int j = i + 1; j < count; j++) {
      if (nets[j].rssi > nets[best].rssi) best = j;
    }
    if (best != i) {
      Net tmp = nets[i]; nets[i] = nets[best]; nets[best] = tmp;
    }
  }

  String json = "[";
  for (int i = 0; i < count; i++) {
    if (i > 0) json += ",";
    json += "{\"ssid\":\"";
    // Minimal JSON-escape for quotes/backslashes in SSIDs
    for (size_t k = 0; k < nets[i].ssid.length(); k++) {
      char c = nets[i].ssid[k];
      if (c == '"' || c == '\\') json += '\\';
      json += c;
    }
    json += "\",\"rssi\":" + String(nets[i].rssi);
    json += ",\"secure\":" + String(nets[i].secure ? "true" : "false");
    json += "}";
  }
  json += "]";

  WiFi.scanDelete();       // free scan result memory
  _scan_kicked_off = false;
  return json;
}

// ============================================================
//  HTML PAGE — baked into firmware, no SPIFFS needed
// ============================================================
static const char AP_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Energy Meter Setup</title>
  <style>
    @import url('https://fonts.googleapis.com/css2?family=DM+Sans:wght@400;500;600&family=DM+Mono&display=swap');
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'DM Sans', sans-serif;
      background: #0a0f1e;
      color: #e8eaf0;
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      padding: 24px;
    }
    .card {
      background: #111827;
      border: 1px solid #1e2d45;
      border-radius: 16px;
      padding: 36px 32px;
      width: 100%;
      max-width: 400px;
      box-shadow: 0 24px 64px rgba(0,0,0,0.5);
    }
    .logo-row {
      display: flex;
      align-items: center;
      gap: 12px;
      margin-bottom: 28px;
    }
    .logo-icon {
      width: 40px; height: 40px;
      background: linear-gradient(135deg, #00d4ff, #0066ff);
      border-radius: 10px;
      display: flex; align-items: center; justify-content: center;
      font-size: 20px; flex-shrink: 0;
    }
    .logo-text { font-size: 18px; font-weight: 600; color: #fff; letter-spacing: -0.3px; }
    .logo-sub  { font-size: 12px; color: #4a6080; font-family: 'DM Mono', monospace; letter-spacing: 0.5px; }
    h1         { font-size: 22px; font-weight: 600; color: #fff; margin-bottom: 6px; letter-spacing: -0.4px; }
    .subtitle  { font-size: 14px; color: #4a6080; margin-bottom: 28px; line-height: 1.5; }
    .field     { margin-bottom: 18px; }
    label      { display: block; font-size: 13px; font-weight: 500; color: #8899aa; margin-bottom: 8px; letter-spacing: 0.3px; }
    .optional  { font-size: 11px; color: #334455; font-weight: 400; margin-left: 6px; }
    input {
      width: 100%;
      padding: 13px 16px;
      background: #0d1520;
      border: 1px solid #1e2d45;
      border-radius: 10px;
      color: #e8eaf0;
      font-size: 15px;
      font-family: 'DM Sans', sans-serif;
      outline: none;
      transition: border-color 0.2s;
      -webkit-appearance: none;
    }
    input:focus        { border-color: #0066ff; }
    input::placeholder { color: #2a3a50; }

    /* 2-way mode toggle */
    .mode-toggle {
      display: flex;
      background: #0d1520;
      border: 1px solid #1e2d45;
      border-radius: 10px;
      overflow: hidden;
    }
    .mode-toggle input[type="radio"] { display: none; }
    .mode-toggle label {
      flex: 1;
      text-align: center;
      padding: 12px 8px;
      font-size: 14px;
      font-weight: 500;
      color: #4a6080;
      cursor: pointer;
      margin: 0;
      letter-spacing: 0;
      transition: color 0.2s, background 0.2s;
    }
    .mode-toggle input[type="radio"]:checked + label {
      background: linear-gradient(135deg, #0055dd, #0033aa);
      color: #fff;
    }

    /* 4-way network picker (GSM) */
    .net-picker {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    .net-picker input[type="radio"] { display: none; }
    .net-picker label {
      flex: 1;
      min-width: calc(50% - 4px);
      text-align: center;
      padding: 11px 6px;
      font-size: 13px;
      font-weight: 500;
      color: #4a6080;
      cursor: pointer;
      margin: 0;
      letter-spacing: 0;
      background: #0d1520;
      border: 1px solid #1e2d45;
      border-radius: 10px;
      transition: color 0.2s, background 0.2s, border-color 0.2s;
    }
    .net-picker input[type="radio"]:checked + label {
      background: linear-gradient(135deg, #0055dd, #0033aa);
      border-color: #0055dd;
      color: #fff;
    }

    /* Network brand colours on checked */
    #net-mtn:checked    + label { background: linear-gradient(135deg, #ffcc00, #e6b800); border-color: #ffcc00; color: #1a1000; }
    #net-glo:checked    + label { background: linear-gradient(135deg, #00a650, #007a3d); border-color: #00a650; color: #fff; }
    #net-9mobile:checked + label { background: linear-gradient(135deg, #7db83a, #5a8a28); border-color: #7db83a; color: #fff; }
    #net-airtel:checked + label { background: linear-gradient(135deg, #e2231a, #b01a12); border-color: #e2231a; color: #fff; }
    #net-other:checked  + label { background: linear-gradient(135deg, #334455, #222f3e); border-color: #445566; color: #e8eaf0; }

    #gsm-section  { display: none; }
    #other-apn    { display: none; margin-top: 12px; }

    /* ---- WiFi scanned-network list ---- */
    #wifi-scan-list {
      display: flex;
      flex-direction: column;
      gap: 6px;
      max-height: 220px;
      overflow-y: auto;
      margin-bottom: 4px;
    }
    .net-row {
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 12px 14px;
      background: #0d1520;
      border: 1px solid #1e2d45;
      border-radius: 10px;
      cursor: pointer;
      transition: background 0.15s, border-color 0.15s;
    }
    .net-row:active { transform: scale(0.99); }
    .net-row.selected {
      background: linear-gradient(135deg, #0055dd, #0033aa);
      border-color: #0055dd;
    }
    .net-row .name {
      flex: 1;
      font-size: 14px;
      font-weight: 500;
      color: #e8eaf0;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .net-row .icons { display: flex; align-items: center; gap: 6px; flex-shrink: 0; }
    .net-row svg { display: block; color: #4a6080; }
    .net-row.selected .name,
    .net-row.selected svg { color: #fff; }

    #scan-status {
      font-size: 13px;
      color: #4a6080;
      text-align: center;
      padding: 18px 0;
    }
    #scan-status .spinner {
      display: inline-block;
      width: 14px; height: 14px;
      border: 2px solid #1e2d45;
      border-top-color: #0066ff;
      border-radius: 50%;
      margin-right: 8px;
      vertical-align: middle;
      animation: spin 0.7s linear infinite;
    }
    @keyframes spin { to { transform: rotate(360deg); } }

    .link-btn {
      background: none;
      border: none;
      color: #0066ff;
      font-size: 13px;
      font-weight: 500;
      font-family: 'DM Sans', sans-serif;
      cursor: pointer;
      padding: 6px 0;
      text-align: center;
      width: 100%;
      display: block;
    }
    .link-btn:active { opacity: 0.7; }

    #manual-ssid-field { display: none; margin-top: 10px; }
    #password-field     { display: none; }

    .device-id {
      font-family: 'DM Mono', monospace;
      font-size: 12px;
      color: #334455;
      text-align: center;
      margin-top: 24px;
      padding: 8px;
      background: #0d1520;
      border-radius: 8px;
      border: 1px solid #1a2535;
    }
    hr { border: none; border-top: 1px solid #1a2535; margin: 24px 0; }
    button.submit-btn {
      width: 100%;
      padding: 15px;
      background: linear-gradient(135deg, #0055dd, #0033aa);
      border: none;
      border-radius: 10px;
      color: #fff;
      font-size: 15px;
      font-weight: 600;
      font-family: 'DM Sans', sans-serif;
      cursor: pointer;
      letter-spacing: -0.2px;
      transition: opacity 0.2s, transform 0.1s;
    }
    button.submit-btn:active { opacity: 0.85; transform: scale(0.99); }
    .input-wrap { position: relative; }
    .input-wrap input { padding-right: 44px; }
    .eye-btn {
      position: absolute; right: 14px; top: 50%;
      transform: translateY(-50%);
      background: none; border: none; cursor: pointer;
      padding: 0; width: auto; color: #4a6080;
      line-height: 1; transition: color 0.2s;
      display: flex; align-items: center; justify-content: center;
    }
    .eye-btn:active { opacity: 1; transform: translateY(-50%) scale(1); }
    #success {
      display: none; text-align: center; padding: 20px 0 8px;
    }
    #success .icon { font-size: 48px; margin-bottom: 16px; }
    #success h2    { font-size: 20px; color: #fff; margin-bottom: 8px; }
    #success p     { font-size: 14px; color: #4a6080; line-height: 1.6; }
    input[type="password"]::-ms-reveal,
    input[type="password"]::-ms-clear,
    input::-webkit-credentials-auto-fill-button,
    input::-webkit-textfield-decoration-container { display: none !important; }
  </style>
</head>
<body>
  <div class="card">
    <div class="logo-row">
      <div class="logo-icon">&#9889;</div>
      <div>
        <div class="logo-text">Energy Meter</div>
        <div class="logo-sub">DEVICE SETUP</div>
      </div>
    </div>

    <div id="form-view">
      <h1>Device Setup</h1>
      <p class="subtitle">Configure connectivity and alerts for this meter.</p>

      <!-- CONNECTION MODE TOGGLE -->
      <div class="field">
        <label>Connection Mode</label>
        <div class="mode-toggle">
          <input type="radio" name="conn_mode" id="mode-wifi" value="0" checked onchange="onModeChange()">
          <label for="mode-wifi">&#128246; WiFi</label>
          <input type="radio" name="conn_mode" id="mode-gsm" value="1" onchange="onModeChange()">
          <label for="mode-gsm">&#128225; GSM</label>
        </div>
      </div>

      <!-- WIFI FIELDS -->
      <div id="wifi-section">
        <div class="field">
          <label>WiFi Network</label>

          <div id="wifi-scan-list"></div>
          <div id="scan-status"><span class="spinner"></span>Scanning for networks&hellip;</div>
          <button type="button" class="link-btn" id="rescan-btn" onclick="startScan()" style="display:none;">&#8635; Rescan</button>
          <button type="button" class="link-btn" id="manual-toggle-btn" onclick="showManualSsid()">Can't see your network? Enter manually</button>

          <div id="manual-ssid-field">
            <input type="text" id="ssid-manual" placeholder="Enter network name"
                   autocomplete="off" autocorrect="off" autocapitalize="none" spellcheck="false"
                   oninput="onManualSsidInput()">
          </div>
        </div>

        <div class="field" id="password-field">
          <label>WiFi Password <span class="optional" id="pw-optional-tag" style="display:none;">leave blank if open network</span></label>
          <div class="input-wrap">
            <input type="password" id="password" placeholder="Enter password" autocomplete="off">
            <button type="button" class="eye-btn" onclick="togglePassword()" id="eye-btn" aria-label="Show password">
              <svg id="eye-open" xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                <path d="M1 12s4-8 11-8 11 8 11 8-4 8-11 8-11-8-11-8z"/>
                <circle cx="12" cy="12" r="3"/>
              </svg>
              <svg id="eye-shut" xmlns="http://www.w3.org/2000/svg" width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="display:none;">
                <path d="M17.94 17.94A10.07 10.07 0 0 1 12 20c-7 0-11-8-11-8a18.45 18.45 0 0 1 5.06-5.94"/>
                <path d="M9.9 4.24A9.12 9.12 0 0 1 12 4c7 0 11 8 11 8a18.5 18.5 0 0 1-2.16 3.19"/>
                <line x1="1" y1="1" x2="23" y2="23"/>
              </svg>
            </button>
          </div>
        </div>
      </div>

      <!-- GSM NETWORK PICKER — shown when GSM selected -->
      <div id="gsm-section">
        <div class="field">
          <label>Mobile Network</label>
          <div class="net-picker">
            <input type="radio" name="network" id="net-mtn"    value="internet" checked onchange="onNetChange()">
            <label for="net-mtn">MTN</label>
            <input type="radio" name="network" id="net-airtel" value="internet" onchange="onNetChange()">
            <label for="net-airtel">Airtel</label>
            <input type="radio" name="network" id="net-glo"    value="glo"      onchange="onNetChange()">
            <label for="net-glo">Glo</label>
            <input type="radio" name="network" id="net-9mobile" value="9mobile" onchange="onNetChange()">
            <label for="net-9mobile">9mobile</label>
            <input type="radio" name="network" id="net-other"  value="other"    onchange="onNetChange()">
            <label for="net-other">Other</label>
          </div>
          <!-- Free-text APN shown only when Other is selected -->
          <div id="other-apn">
            <input type="text" id="apn-custom" placeholder="Enter APN (e.g. mms.ntel.com.ng)"
                   autocomplete="off" autocorrect="off" autocapitalize="none" spellcheck="false">
          </div>
        </div>
      </div>

      <hr>

      <!-- PHONE -->
      <div class="field">
        <label>Phone Number <span class="optional">optional &mdash; for low-unit SMS alerts</span></label>
        <input type="tel" id="phone" placeholder="e.g. 08012345678">
      </div>

      <button class="submit-btn" onclick="submitForm()">Save &amp; Connect</button>
    </div>

    <div id="success">
      <div class="icon">&#10003;</div>
      <h2>Setup Complete</h2>
      <p id="success-msg">Your meter is now connecting.<br>You can close this page.</p>
    </div>

    <div class="device-id" id="did">Smart Meter</div>
  </div>

  <script>
    fetch('/deviceid').then(r=>r.text()).then(id=>{
      document.getElementById('did').textContent = id;
    }).catch(()=>{});

    // ---- selected network state ----
    var selectedSsid   = null;   // from the tapped list, or null if manual entry is used
    var manualMode     = false;
    var scanPollTimer  = null;
    var scanAttempts   = 0;

    function onModeChange() {
      var gsm = document.getElementById('mode-gsm').checked;
      document.getElementById('wifi-section').style.display = gsm ? 'none'  : 'block';
      document.getElementById('gsm-section').style.display  = gsm ? 'block' : 'none';
      if (!gsm && !scanPollTimer) startScan();
    }

    function onNetChange() {
      var other = document.getElementById('net-other').checked;
      document.getElementById('other-apn').style.display = other ? 'block' : 'none';
    }

    function getApn() {
      var other = document.getElementById('net-other').checked;
      if (other) {
        var custom = document.getElementById('apn-custom').value.trim();
        return custom || 'internet';
      }
      var nets = document.getElementsByName('network');
      for (var i = 0; i < nets.length; i++) {
        if (nets[i].checked) return nets[i].value;
      }
      return 'internet';
    }

    // ---- WiFi scanning ----
    function signalBarsSvg(rssi) {
      // rssi roughly: >-55 great, >-67 good, >-78 fair, else weak
      var level = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
      var bars = '';
      for (var b = 1; b <= 4; b++) {
        var h = 4 + b * 3;
        var y = 18 - h;
        var on = b <= level ? 'currentColor' : '#26344a';
        bars += '<rect x="' + (b * 5 - 3) + '" y="' + y + '" width="3" height="' + h + '" fill="' + on + '" rx="0.5"/>';
      }
      return '<svg width="20" height="18" viewBox="0 0 20 18">' + bars + '</svg>';
    }

    var lockSvg = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>';

    function renderNetworkList(networks) {
      var list = document.getElementById('wifi-scan-list');
      list.innerHTML = '';

      if (networks.length === 0) {
        document.getElementById('scan-status').innerHTML = 'No networks found nearby.';
        document.getElementById('rescan-btn').style.display = 'block';
        return;
      }

      document.getElementById('scan-status').style.display = 'none';
      document.getElementById('rescan-btn').style.display = 'block';

      networks.forEach(function(net) {
        var row = document.createElement('div');
        row.className = 'net-row';
        row.innerHTML =
          '<span class="icons">' + signalBarsSvg(net.rssi) + '</span>' +
          '<span class="name">' + escapeHtml(net.ssid) + '</span>' +
          '<span class="icons">' + (net.secure ? lockSvg : '') + '</span>';
        row.onclick = function() { selectNetwork(net.ssid, net.secure, row); };
        list.appendChild(row);
      });
    }

    function escapeHtml(s) {
      var d = document.createElement('div');
      d.textContent = s;
      return d.innerHTML;
    }

    function selectNetwork(ssid, secure, rowEl) {
      manualMode   = false;
      selectedSsid = ssid;

      var rows = document.querySelectorAll('.net-row');
      rows.forEach(function(r) { r.classList.remove('selected'); });
      rowEl.classList.add('selected');

      document.getElementById('manual-ssid-field').style.display = 'none';
      document.getElementById('ssid-manual').value = '';

      document.getElementById('password-field').style.display = 'block';
      document.getElementById('pw-optional-tag').style.display = secure ? 'none' : 'inline';
      document.getElementById('password').focus();
    }

    function showManualSsid() {
      manualMode   = true;
      selectedSsid = null;
      var rows = document.querySelectorAll('.net-row');
      rows.forEach(function(r) { r.classList.remove('selected'); });

      document.getElementById('manual-ssid-field').style.display = 'block';
      document.getElementById('password-field').style.display = 'block';
      document.getElementById('pw-optional-tag').style.display = 'inline';
      document.getElementById('ssid-manual').focus();
    }

    function onManualSsidInput() {
      manualMode = document.getElementById('ssid-manual').value.trim().length > 0;
    }

    function startScan() {
      scanAttempts = 0;
      document.getElementById('wifi-scan-list').innerHTML = '';
      document.getElementById('scan-status').style.display = 'block';
      document.getElementById('scan-status').innerHTML = '<span class="spinner"></span>Scanning for networks&hellip;';
      document.getElementById('rescan-btn').style.display = 'none';
      fetch('/scan');
      if (scanPollTimer) clearTimeout(scanPollTimer);
      pollScanResults();
    }

    function pollScanResults() {
      fetch('/scanresults').then(function(r) { return r.text(); }).then(function(txt) {
        scanAttempts++;
        if (txt === '') {
          // still scanning — try again, give up after ~15s
          if (scanAttempts < 15) {
            scanPollTimer = setTimeout(pollScanResults, 1000);
          } else {
            document.getElementById('scan-status').innerHTML = 'Scan timed out.';
            document.getElementById('rescan-btn').style.display = 'block';
          }
          return;
        }
        var networks = JSON.parse(txt);
        renderNetworkList(networks);
      }).catch(function() {
        scanPollTimer = setTimeout(pollScanResults, 1500);
      });
    }

    // Kick off scanning immediately on page load (WiFi mode is the default tab)
    startScan();

    function togglePassword() {
      var input   = document.getElementById('password');
      var eyeOpen = document.getElementById('eye-open');
      var eyeShut = document.getElementById('eye-shut');
      var btn     = document.getElementById('eye-btn');
      if (input.type === 'password') {
        input.type = 'text';
        eyeOpen.style.display = 'none';
        eyeShut.style.display = 'block';
        btn.style.color = '#0066ff';
      } else {
        input.type = 'password';
        eyeOpen.style.display = 'block';
        eyeShut.style.display = 'none';
        btn.style.color = '#4a6080';
      }
    }

    function submitForm() {
      var gsm      = document.getElementById('mode-gsm').checked;
      var connMode = gsm ? '1' : '0';
      var ssid     = manualMode ? document.getElementById('ssid-manual').value.trim() : (selectedSsid || '');
      var password = document.getElementById('password').value;
      var apn      = getApn();
      var phone    = document.getElementById('phone').value.trim();

      if (!gsm && !ssid) {
        alert('Please select or enter your WiFi network name.');
        return;
      }
      if (gsm && document.getElementById('net-other').checked) {
        var custom = document.getElementById('apn-custom').value.trim();
        if (!custom) {
          alert('Please enter the APN for your network, or select a network from the list.');
          return;
        }
      }

      var body = 'conn_mode=' + encodeURIComponent(connMode) +
                 '&ssid='     + encodeURIComponent(ssid) +
                 '&password=' + encodeURIComponent(password) +
                 '&apn='      + encodeURIComponent(apn) +
                 '&phone='    + encodeURIComponent(phone);

      fetch('/save', {
        method:  'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body:    body
      }).then(function(r){ return r.text(); }).then(function(){
        document.getElementById('form-view').style.display = 'none';
        var msg = gsm
          ? 'Your meter is now connecting via GSM.<br>You can close this page.'
          : 'Your meter is now connecting to WiFi.<br>You can close this page.';
        document.getElementById('success-msg').innerHTML = msg;
        document.getElementById('success').style.display = 'block';
      }).catch(function(){ alert('Error saving. Please try again.'); });
    }
  </script>
</body>
</html>
)rawhtml";

// ============================================================
//  ROUTE HANDLERS
// ============================================================
static void _handle_root() {
  _server.send_P(200, "text/html", AP_HTML);
}

static void _handle_device_id() {
  _server.send(200, "text/plain", String(_ap_ssid));
}

static void _handle_scan_trigger() {
  // Only kicks off a fresh scan if one isn't already running — see
  // _wifi_scan_results_json() for the "already scanning" check via
  // WiFi.scanComplete(). Calling this repeatedly (e.g. Rescan button)
  // is safe.
  int status = WiFi.scanComplete();
  if (status != WIFI_SCAN_RUNNING) {
    _wifi_scan_start();
  }
  _server.send(200, "text/plain", "OK");
}

static void _handle_scan_results() {
  // Empty body = "still scanning, poll again" (handled client-side).
  // Non-empty = JSON array of networks, scan complete.
  String json = _wifi_scan_results_json();
  _server.send(200, "application/json", json);
}

static void _handle_save() {
  String conn_mode_str = _server.arg("conn_mode");
  String ssid          = _server.arg("ssid");
  String password      = _server.arg("password");
  String apn           = _server.arg("apn");
  String phone         = _server.arg("phone");

  uint8_t conn_mode = (conn_mode_str == "1") ? CONN_MODE_GSM : CONN_MODE_WIFI;

  // WiFi mode requires SSID
  if (conn_mode == CONN_MODE_WIFI && ssid.length() == 0) {
    _server.send(400, "text/plain", "Missing SSID");
    return;
  }

  // Save connectivity mode
  storage_saveConnMode(conn_mode);

  if (conn_mode == CONN_MODE_WIFI) {
    storage_saveWifiCredentials(ssid.c_str(), password.c_str());
    Serial.print("[AP] WiFi credentials saved — SSID: ");
    Serial.println(ssid);
  } else {
    // Save APN — overrides firmware cycling list on first connect
    if (apn.length() == 0) apn = "internet";
    storage_saveGsmApn(apn.c_str());
    Serial.print("[AP] GSM APN saved: ");
    Serial.println(apn);
  }

  if (phone.length() > 0) {
    storage_savePhoneNumber(phone.c_str());
    Serial.print("[AP] Phone saved: ");
    Serial.println(phone);
  }

  Serial.print("[AP] Credentials saved — mode: ");
  Serial.println(conn_mode == CONN_MODE_WIFI ? "WiFi" : "GSM");

  // Respond first, then mark complete so the response actually sends
  _server.send(200, "text/plain", "OK");
  delay(300);
  _is_complete = true;
}

// Captive portal — redirect all unknown URLs to root
static void _handle_not_found() {
  _server.sendHeader("Location", "http://192.168.4.1", true);
  _server.send(302, "text/plain", "");
}

// ============================================================
//  PUBLIC API
// ============================================================
void apProvision_start(const char* device_id) {
  Serial.println("[AP] Starting AP provisioning mode");

  _is_complete = false;
  _timed_out   = false;
  _start_time  = millis();
  _scan_kicked_off = false;

  strncpy(_ap_ssid, device_id, sizeof(_ap_ssid) - 1);

  // WIFI_AP_STA (not plain WIFI_AP) — required so the STA radio can
  // scan for nearby networks while the AP radio keeps serving the
  // captive portal to the connected phone.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(_ap_ssid);
  delay(100);

  IPAddress ap_ip = WiFi.softAPIP();
  Serial.print("[AP] Network SSID: ");
  Serial.println(_ap_ssid);
  Serial.print("[AP] IP address:   ");
  Serial.println(ap_ip);

  _dns.start(53, "*", ap_ip);

  _server.on("/",            HTTP_GET,  _handle_root);
  _server.on("/deviceid",    HTTP_GET,  _handle_device_id);
  _server.on("/scan",        HTTP_GET,  _handle_scan_trigger);
  _server.on("/scanresults", HTTP_GET,  _handle_scan_results);
  _server.on("/save",        HTTP_POST, _handle_save);
  _server.onNotFound(_handle_not_found);
  _server.begin();

  // Kick off a scan immediately so results are likely ready by the
  // time the phone opens the page (page JS also triggers/polls on
  // load regardless, so this is just a head start).
  _wifi_scan_start();

  Serial.println("[AP] Connect phone to \"" + String(_ap_ssid) + "\" — browser will open automatically");
  Serial.print("[AP] Timeout in ");
  Serial.print(AP_TIMEOUT_MS / 1000);
  Serial.println("s");
}

void apProvision_loop() {
  if (_is_complete || _timed_out) return;
  _dns.processNextRequest();
  _server.handleClient();
  if (millis() - _start_time >= AP_TIMEOUT_MS) {
    Serial.println("[AP] Timed out");
    _timed_out = true;
  }
}

void apProvision_stop() {
  Serial.println("[AP] Stopping");
  _server.stop();
  _dns.stop();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  delay(200);
  Serial.println("[AP] Stopped");
}

bool apProvision_isComplete() { return _is_complete; }
bool apProvision_timedOut()   { return _timed_out; }