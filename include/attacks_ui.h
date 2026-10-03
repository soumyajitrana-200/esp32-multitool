#pragma once
// SOUMYA Gadget — Attacks Web UI
// Served at http://192.168.4.1/attacks

const char ATTACKS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>SOUMYA Attacks</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{background:#050505;color:#fff;font-family:-apple-system,'Segoe UI',sans-serif;padding:12px;min-height:100vh}
.header{display:flex;justify-content:space-between;align-items:center;padding:8px 0 14px;border-bottom:1px solid #ff336633;margin-bottom:12px}
.header h1{font-size:15px;color:#ff3366;letter-spacing:2px;font-weight:700}
.emergency-btn{background:#ff3366;border:none;color:#fff;padding:8px 12px;border-radius:8px;font-size:11px;font-weight:700;letter-spacing:1px}
.emergency-btn:active{transform:scale(0.95)}
.warning{background:#ff336611;border:1px solid #ff336644;border-radius:10px;padding:9px 12px;font-size:10px;color:#ff99aa;margin-bottom:12px;text-align:center;letter-spacing:1px}
.card{background:#0d0d0d;border:1px solid #1a1a1a;border-radius:14px;padding:14px;margin-bottom:12px}
.card h3{font-size:11px;color:#ff3366;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:12px;font-weight:600}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.grid button{background:#1a1a1a;border:1px solid #222;color:#ccc;padding:12px 4px;border-radius:10px;font-size:10px;font-weight:600}
.grid button:active{background:#ff3366;color:#fff;transform:scale(0.94)}
.status{display:none;background:#ff3366;color:#fff;padding:10px 14px;border-radius:10px;font-size:12px;font-weight:700;text-align:center;margin-bottom:12px;letter-spacing:1px;animation:pulse 1.4s infinite}
.status.show{display:block}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.65}}
.status .stop{background:rgba(0,0,0,.35);border:none;color:#fff;padding:4px 10px;border-radius:6px;font-size:11px;margin-left:8px}
.ap{display:flex;align-items:center;gap:10px;padding:10px 0;border-bottom:1px solid #151515}
.ap:last-child{border-bottom:none}
.ap .info{flex:1;min-width:0}
.ap .ssid{font-size:12px;font-weight:500;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.ap .meta{font-size:10px;color:#666;margin-top:2px;font-family:monospace}
.ap .meta .sig{color:#00e5ff}
.ap .deauth-btn{background:#ff336622;border:1px solid #ff336655;color:#ff3366;padding:6px 10px;border-radius:6px;font-size:10px;font-weight:700}
.ap .deauth-btn:active{background:#ff3366;color:#fff}
.counter{text-align:center;padding:12px 0}
.counter .num{font-size:30px;font-weight:700;color:#ff3366;font-family:monospace}
.counter .label{font-size:10px;color:#666;text-transform:uppercase;letter-spacing:1px;margin-top:4px}
.empty{text-align:center;color:#555;font-size:11px;padding:18px;font-style:italic}
.footer{text-align:center;font-size:10px;color:#444;padding:14px 0 6px}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(100px);background:#ff3366;color:#fff;padding:10px 20px;border-radius:20px;font-size:12px;font-weight:600;transition:.3s;z-index:999}
.toast.show{transform:translateX(-50%) translateY(0)}
.toast.ok{background:#00e5ff;color:#000}
.modal{display:none;position:fixed;inset:0;background:rgba(0,0,0,.85);z-index:1000;align-items:center;justify-content:center;padding:24px}
.modal.show{display:flex}
.modal-box{background:#0d0d0d;border:1px solid #ff336644;border-radius:14px;padding:20px;max-width:320px;width:100%;text-align:center}
.modal-box h2{color:#ff3366;font-size:15px;margin-bottom:10px;letter-spacing:2px}
.modal-box p{color:#aaa;font-size:12px;line-height:1.6;margin-bottom:18px}
.modal-box .actions{display:flex;gap:10px}
.modal-box button{flex:1;padding:12px;border-radius:8px;border:none;font-size:13px;font-weight:700}
.modal-box .cancel{background:#1a1a1a;color:#fff}
.modal-box .confirm{background:#ff3366;color:#fff}
</style>
</head>
<body>

<div class="header">
<h1>SOUMYA ATTACKS</h1>
<button class="emergency-btn" onclick="showEmergency()">EXIT</button>
</div>

<div class="warning">&#9888; AUTHORIZED SECURITY TESTING ONLY</div>

<div class="status" id="status">
<span id="status-text">ATTACK ACTIVE</span>
<button class="stop" onclick="stopAll()">STOP</button>
</div>

<div class="card" id="counter-card" style="display:none">
<h3>Live Counter</h3>
<div class="counter">
<div class="num" id="pkt-count">0</div>
<div class="label">packets sent</div>
</div>
</div>

<div class="card">
<h3>WiFi Tools</h3>
<div class="grid">
<button onclick="wifiScan()">Scan WiFi</button>
<button onclick="startAttack('beacon',-1,'Beacon Spam')">Beacon Spam</button>
<button onclick="startAttack('probe',-1,'Probe Flood')">Probe Flood</button>
</div>
</div>

<div class="card">
<h3>Scan Results (<span id="ap-count">0</span>)</h3>
<div id="ap-list"><div class="empty">Tap "Scan WiFi" to find networks</div></div>
</div>

<div class="card">
<h3>Bluetooth Tools</h3>
<div class="grid">
<button onclick="toast('BLE Scan - OLED only')">BLE Scan</button>
<button onclick="toast('Classic BT - OLED only')">Classic BT</button>
<button onclick="toast('BLE Spam - OLED only')">BLE Spam</button>
</div>
</div>

<div class="card">
<h3>IR Tools</h3>
<div class="grid">
<button onclick="toast('IR Learn - OLED only')">IR Learn</button>
<button onclick="toast('IR Transmit - OLED only')">IR Transmit</button>
<button onclick="toast('IR Jammer - OLED only')">IR Jammer</button>
</div>
</div>

<div class="footer">SOUMYA Gadget v9.1 &bull; Attacks</div>
<div class="toast" id="toast">OK</div>

<div class="modal" id="emergency-modal">
<div class="modal-box">
<h2>&#9888; EMERGENCY EXIT</h2>
<p>Stop all attacks, kill AP, return to Main Menu silently.</p>
<div class="actions">
<button class="cancel" onclick="hideEmergency()">Cancel</button>
<button class="confirm" onclick="doEmergency()">Confirm</button>
</div>
</div>
</div>

<script>
var $=function(i){return document.getElementById(i)};

function esc(s){return s.replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}

function toast(m,ok){
var t=$('toast');
t.textContent=m;
t.className='toast'+(ok?' ok':'');
t.classList.add('show');
clearTimeout(t._t);
t._t=setTimeout(function(){t.classList.remove('show')},1800);
}

var attackRunning=false;
var pollTimer=null;
var scanPollTimer=null;

function wifiScan(){
$('ap-list').innerHTML='<div class="empty">Scanning&hellip;</div>';
$('ap-count').textContent='0';
fetch('/atk/scan').catch(function(){toast('Scan failed',1);return});
var tries=0;
clearInterval(scanPollTimer);
scanPollTimer=setInterval(function(){
tries++;
fetch('/atk/scanresults').then(function(r){return r.json()}).then(function(j){
if(!j.ready){
if(tries>30){clearInterval(scanPollTimer);toast('Scan timeout',1)}
return;
}
clearInterval(scanPollTimer);
renderAPs(j.aps);
toast('Found '+j.count+' networks',1);
}).catch(function(){clearInterval(scanPollTimer);toast('Scan error',1)});
},400);
}

function renderAPs(list){
$('ap-count').textContent=list.length;
if(!list.length){
$('ap-list').innerHTML='<div class="empty">No networks found</div>';
return;
}
$('ap-list').innerHTML=list.map(function(ap){
var bars=ap.rssi>-50?'&#9646;&#9646;&#9646;&#9646;':ap.rssi>-70?'&#9646;&#9646;&#9646;':ap.rssi>-85?'&#9646;&#9646;':'&#9646;';
return '<div class="ap">'+
'<div class="info"><div class="ssid">'+esc(ap.ssid)+'</div>'+
'<div class="meta"><span class="sig">'+bars+'</span> '+ap.rssi+' dBm &bull; CH '+ap.ch+'</div></div>'+
'<button class="deauth-btn" onclick="startAttack(\'deauth\','+ap.idx+',\'Deauth '+esc(ap.ssid)+'\')">DEAUTH</button>'+
'</div>';
}).join('');
}

function startAttack(cmd,idx,name){
fetch('/atk/cmd?cmd='+cmd+'&idx='+idx).catch(function(){toast('Command failed',1);return});
attackRunning=true;
$('status').classList.add('show');
$('status-text').textContent=name.toUpperCase()+' ACTIVE';
$('counter-card').style.display='block';
toast(name+' started');
if(pollTimer)clearInterval(pollTimer);
pollTimer=setInterval(pollStatus,800);
}

function pollStatus(){
fetch('/atk/status').then(function(r){return r.json()}).then(function(j){
$('pkt-count').textContent=j.packets;
if(!j.running&&attackRunning)stopAll(true);
}).catch(function(){});
}

function stopAll(silent){
attackRunning=false;
if(pollTimer){clearInterval(pollTimer);pollTimer=null}
$('status').classList.remove('show');
$('counter-card').style.display='none';
fetch('/atk/cmd?cmd=stop').catch(function(){});
if(!silent)toast('Stopped',1);
}

function showEmergency(){$('emergency-modal').classList.add('show')}
function hideEmergency(){$('emergency-modal').classList.remove('show')}
function doEmergency(){
hideEmergency();
stopAll(true);
fetch('/atk/emergency',{method:'POST'}).catch(function(){});
document.body.innerHTML='<div style="text-align:center;padding:80px 20px;color:#00e5ff;font-family:sans-serif">'+
'<h2>Device reset.</h2><p style="color:#888;margin-top:12px">You can close this page.</p></div>';
}
</script>
</body>
</html>
)rawliteral";
