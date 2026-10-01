#ifndef ATTACKS_UI_H
#define ATTACKS_UI_H

const char ATTACKS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>SOUMYA Attacks</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body { background: #050505; color: #fff; font-family: 'Segoe UI', -apple-system, sans-serif; padding: 12px; min-height: 100vh; }
  .header { display: flex; justify-content: space-between; align-items: center; padding: 8px 0 16px; border-bottom: 1px solid #ff336633; margin-bottom: 16px; }
  .header h1 { font-size: 16px; color: #ff3366; letter-spacing: 2px; text-shadow: 0 0 8px #ff336666; }
  .emergency-btn { background: #ff3366; border: none; color: #fff; padding: 8px 14px; border-radius: 8px; font-size: 11px; font-weight: 700; letter-spacing: 1px; box-shadow: 0 0 12px #ff336688; }
  .warning { background: #ff336611; border: 1px solid #ff336644; border-radius: 10px; padding: 10px 14px; font-size: 11px; color: #ff99aa; margin-bottom: 14px; text-align: center; letter-spacing: 1px; }
  .card { background: #0d0d0d; border: 1px solid #1a1a1a; border-radius: 14px; padding: 16px; margin-bottom: 14px; }
  .card h3 { font-size: 12px; color: #ff3366; text-transform: uppercase; letter-spacing: 1.5px; margin-bottom: 12px; font-weight: 600; }
  .btn-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
  .btn-grid button { background: #1a1a1a; border: 1px solid #222; color: #ccc; padding: 12px 4px; border-radius: 10px; font-size: 11px; font-weight: 600; transition: 0.2s; }
  .btn-grid button:active { background: #ff3366; color: #fff; }
  .status { display: none; background: #ff3366; color: #fff; padding: 10px 14px; border-radius: 10px; font-size: 12px; font-weight: 700; text-align: center; margin-bottom: 14px; letter-spacing: 1px; }
  .status.show { display: block; }
  .status .stop { background: rgba(0,0,0,0.3); border: none; color: #fff; padding: 4px 10px; border-radius: 6px; font-size: 11px; margin-left: 8px; }
  .ap { display: flex; align-items: center; gap: 10px; padding: 10px 0; border-bottom: 1px solid #151515; }
  .ap:last-child { border-bottom: none; }
  .ap .info { flex: 1; min-width: 0; }
  .ap .ssid { font-size: 13px; font-weight: 500; }
  .ap .meta { font-size: 11px; color: #666; margin-top: 2px; }
  .ap .meta .sig { color: #00e5ff; }
  .ap .deauth-btn { background: #ff336622; border: 1px solid #ff336655; color: #ff3366; padding: 6px 12px; border-radius: 6px; font-size: 11px; font-weight: 600; }
  .counter { text-align: center; padding: 12px 0; }
  .counter .num { font-size: 32px; font-weight: 700; color: #ff3366; text-shadow: 0 0 15px #ff336688; font-family: monospace; }
  .counter .label { font-size: 11px; color: #666; text-transform: uppercase; letter-spacing: 1px; margin-top: 4px; }
  .empty { text-align: center; color: #555; font-size: 12px; padding: 20px; }
  .footer { text-align: center; font-size: 10px; color: #444; padding: 16px 0 8px; }
  .toast { position: fixed; bottom: 24px; left: 50%; transform: translateX(-50%) translateY(100px); background: #ff3366; color: #fff; padding: 10px 20px; border-radius: 20px; font-size: 12px; font-weight: 600; transition: 0.3s; z-index: 999; }
  .toast.show { transform: translateX(-50%) translateY(0); }
  .toast.ok { background: #00e5ff; color: #000; }
  .modal { display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.85); z-index: 1000; align-items: center; justify-content: center; padding: 24px; }
  .modal.show { display: flex; }
  .modal-box { background: #0d0d0d; border: 1px solid #ff336644; border-radius: 14px; padding: 20px; max-width: 340px; width: 100%; text-align: center; }
  .modal-box h2 { color: #ff3366; font-size: 16px; margin-bottom: 12px; letter-spacing: 2px; }
  .modal-box p { color: #aaa; font-size: 12px; line-height: 1.6; margin-bottom: 20px; }
  .modal-box .actions { display: flex; gap: 10px; }
  .modal-box button { flex: 1; padding: 12px; border-radius: 8px; border: none; font-size: 13px; font-weight: 700; }
  .modal-box .cancel { background: #1a1a1a; color: #fff; }
  .modal-box .confirm { background: #ff3366; color: #fff; }
</style>
</head>
<body>

<div class="header">
  <h1>SOUMYA ATTACKS</h1>
  <button class="emergency-btn" onclick="showEmergency()">🚨 EXIT</button>
</div>

<div class="warning">⚠ AUTHORIZED SECURITY TESTING ONLY</div>

<div class="status" id="status">
  <span id="status-text">ATTACK ACTIVE</span>
  <button class="stop" onclick="stopAll()">STOP</button>
</div>

<div class="card" id="counter-card" style="display:none;">
  <h3>Live Counter</h3>
  <div class="counter">
    <div class="num" id="pkt-count">0</div>
    <div class="label">packets sent</div>
  </div>
</div>

<div class="card">
  <h3>📡 WiFi Tools</h3>
  <div class="btn-grid">
    <button onclick="wifiScan()" id="btn-scan">Scan WiFi</button>
    <button onclick="startAttack('beacon', -1, 'Beacon Spam')">Beacon Spam</button>
    <button onclick="startAttack('probe', -1, 'Probe Flood')">Probe Flood</button>
  </div>
</div>

<div class="card">
  <h3>Scan Results (<span id="ap-count">0</span>)</h3>
  <div id="ap-list"><div class="empty">Tap "Scan WiFi" to find networks</div></div>
</div>

<div class="card">
  <h3>🎧 Bluetooth Tools</h3>
  <div class="btn-grid">
    <button onclick="toast('BLE Scan — use OLED menu')">BLE Scan</button>
    <button onclick="toast('Classic BT — use OLED menu')">Classic BT</button>
    <button onclick="toast('BLE Spam — use OLED menu')">BLE Spam</button>
  </div>
</div>

<div class="card">
  <h3>📡 IR Tools</h3>
  <div class="btn-grid">
    <button onclick="toast('IR — use OLED menu')">IR Learn</button>
    <button onclick="toast('IR — use OLED menu')">IR Transmit</button>
    <button onclick="toast('IR — use OLED menu')">IR Jammer</button>
  </div>
</div>

<div class="footer">SOUMYA Gadget v9.0 • Attacks Panel</div>
<div class="toast" id="toast">OK</div>

<div class="modal" id="emergency-modal">
  <div class="modal-box">
    <h2>⚠ EMERGENCY EXIT</h2>
    <p>Stops all attacks, kills the AP, returns to Main Menu silently.</p>
    <div class="actions">
      <button class="cancel" onclick="hideEmergency()">Cancel</button>
      <button class="confirm" onclick="doEmergency()">Confirm</button>
    </div>
  </div>
</div>

<script>
let attackRunning = false;
let statusInterval = null;
function toast(msg, ok) {
  const t = document.getElementById('toast');
  t.innerText = msg;
  t.classList.toggle('ok', !!ok);
  t.classList.add('show');
  clearTimeout(window._toastT);
  window._toastT = setTimeout(() => t.classList.remove('show'), 2000);
}
function api(url) { return fetch(url).then(r => r.json()).catch(e => null); }
function wifiScan() {
  const btn = document.getElementById('btn-scan');
  btn.disabled = true;
  btn.innerText = 'Scanning...';
  fetch('/scan').then(r => r.json()).then(data => {
    document.getElementById('ap-count').innerText = data.count;
    const list = document.getElementById('ap-list');
    if (!data.aps || data.aps.length === 0) {
      list.innerHTML = '<div class="empty">No networks found</div>';
    } else {
      let html = '';
      data.aps.forEach(ap => {
        const sigBars = ap.rssi > -50 ? '▮▮▮▮' : ap.rssi > -70 ? '▮▮▮' : ap.rssi > -85 ? '▮▮' : '▮';
        html += '<div class="ap">';
        html += '<div class="info">';
        html += '<div class="ssid">' + (ap.ssid || '(hidden)') + '</div>';
        html += '<div class="meta"><span class="sig">' + sigBars + '</span> ' + ap.rssi + ' dBm • CH ' + ap.ch + '</div>';
        html += '</div>';
        html += '<button class="deauth-btn" onclick="startAttack(\'deauth\',' + ap.idx + ', \'Deauth ' + (ap.ssid || 'hidden').substring(0,10) + '\')">DEAUTH</button>';
        html += '</div>';
      });
      list.innerHTML = html;
    }
    btn.disabled = false;
    btn.innerText = 'Scan WiFi';
    toast('Found ' + data.count + ' networks', true);
  }).catch(e => { btn.disabled = false; btn.innerText = 'Scan WiFi'; toast('Scan failed'); });
}
function startAttack(cmd, idx, name) {
  let url = '/cmd?cmd=' + cmd;
  if (idx >= 0) url += '&idx=' + idx;
  fetch(url).then(() => {
    attackRunning = true;
    document.getElementById('status').classList.add('show');
    document.getElementById('status-text').innerText = name.toUpperCase() + ' ACTIVE';
    document.getElementById('counter-card').style.display = 'block';
    toast(name + ' started');
    pollStatus();
  });
}
function stopAll() {
  fetch('/cmd?cmd=stop').then(() => {
    attackRunning = false;
    document.getElementById('status').classList.remove('show');
    document.getElementById('counter-card').style.display = 'none';
    toast('Stopped', true);
  });
}
function pollStatus() {
  if (statusInterval) clearInterval(statusInterval);
  statusInterval = setInterval(() => {
    api('/status').then(data => {
      if (!data) return;
      document.getElementById('pkt-count').innerText = data.packets || 0;
      if (!data.running && attackRunning) {
        attackRunning = false;
        document.getElementById('status').classList.remove('show');
        document.getElementById('counter-card').style.display = 'none';
        clearInterval(statusInterval);
      }
    });
  }, 1000);
}
function showEmergency() { document.getElementById('emergency-modal').classList.add('show'); }
function hideEmergency() { document.getElementById('emergency-modal').classList.remove('show'); }
function doEmergency() {
  fetch('/emergency', { method: 'POST' }).then(() => {
    hideEmergency();
    toast('Emergency exit sent', true);
    setTimeout(() => {
      document.body.innerHTML = '<div style="text-align:center;padding:60px 20px;color:#00e5ff;font-family:sans-serif;"><h2>Device reset.</h2><p style="color:#888;margin-top:12px;">You can close this page.</p></div>';
    }, 1000);
  });
}
pollStatus();
</script>
</body>
</html>
)rawliteral";

#endif
