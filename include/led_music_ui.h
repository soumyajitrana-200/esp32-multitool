#ifndef LED_MUSIC_UI_H
#define LED_MUSIC_UI_H

const char LED_MUSIC_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>SOUMYA Gadget — LED + Music</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body { background: #050505; color: #fff; font-family: -apple-system, 'Segoe UI', sans-serif; padding: 12px; min-height: 100vh; }
  .header { display: flex; justify-content: space-between; align-items: center; padding: 8px 0 16px; border-bottom: 1px solid #00e5ff33; margin-bottom: 14px; }
  .header h1 { font-size: 15px; color: #00e5ff; letter-spacing: 2px; text-shadow: 0 0 10px #00e5ff66; font-weight: 700; }
  .header .ip { font-size: 11px; color: #666; font-family: monospace; }
  .card { background: #0d0d0d; border: 1px solid #1a1a1a; border-radius: 14px; padding: 14px; margin-bottom: 12px; }
  .card h3 { font-size: 11px; color: #00e5ff; text-transform: uppercase; letter-spacing: 1.5px; margin-bottom: 12px; font-weight: 600; display: flex; justify-content: space-between; align-items: center; }
  .card h3 .badge { background: #00e5ff22; color: #00e5ff; font-size: 10px; padding: 2px 8px; border-radius: 10px; font-weight: 700; letter-spacing: 0; }
  .player { display: flex; gap: 12px; align-items: center; }
  .cover { width: 60px; height: 60px; border-radius: 10px; background: linear-gradient(135deg, #00e5ff22, #ff00aa22); border: 1px solid #00e5ff44; display: flex; align-items: center; justify-content: center; font-size: 22px; flex-shrink: 0; }
  .track { flex: 1; min-width: 0; }
  .track .title { font-size: 14px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; margin-bottom: 3px; }
  .track .time { font-size: 10px; color: #888; margin-bottom: 8px; }
  .controls { display: flex; gap: 6px; }
  .controls button { background: #1a1a1a; border: none; color: #fff; width: 34px; height: 34px; border-radius: 8px; font-size: 13px; transition: 0.2s; }
  .controls button:active { background: #00e5ff; color: #000; }
  .controls button.play { background: #00e5ff; color: #000; }
  .wheel-wrap { display: flex; justify-content: center; padding: 10px 0; }
  canvas#wheel { width: 170px; height: 170px; border-radius: 50%; box-shadow: 0 0 20px #00e5ff33; cursor: pointer; }
  .slider-row { display: flex; align-items: center; gap: 10px; margin: 10px 0; }
  .slider-row label { font-size: 11px; color: #999; min-width: 65px; }
  .slider-row .val { font-size: 12px; color: #00e5ff; min-width: 35px; text-align: right; font-weight: 600; font-family: monospace; }
  input[type=range] { -webkit-appearance: none; flex: 1; background: #1a1a1a; height: 5px; border-radius: 3px; outline: none; }
  input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 18px; height: 18px; background: #00e5ff; border-radius: 50%; box-shadow: 0 0 10px #00e5ff; cursor: pointer; }
  .num-row { display: flex; align-items: center; gap: 10px; margin: 10px 0; }
  .num-row label { font-size: 11px; color: #999; min-width: 65px; }
  .num-row .num-input-group { flex: 1; display: flex; align-items: center; gap: 6px; }
  .num-row button { background: #1a1a1a; border: 1px solid #222; color: #00e5ff; width: 38px; height: 38px; border-radius: 8px; font-size: 18px; font-weight: 700; transition: 0.2s; }
  .num-row button:active { background: #00e5ff; color: #000; transform: scale(0.92); }
  .num-row input[type=number] { flex: 1; background: #0a0a0a; border: 1px solid #00e5ff44; color: #00e5ff; padding: 10px 12px; border-radius: 8px; font-size: 15px; font-weight: 700; font-family: monospace; text-align: center; outline: none; transition: 0.2s; -moz-appearance: textfield; }
  .num-row input[type=number]:focus { border-color: #00e5ff; box-shadow: 0 0 12px #00e5ff44; background: #0f1a1a; }
  .num-row input[type=number]::-webkit-outer-spin-button, .num-row input[type=number]::-webkit-inner-spin-button { -webkit-appearance: none; margin: 0; }
  .grid-scroll { max-height: 320px; overflow-y: auto; padding-right: 4px; }
  .grid-scroll::-webkit-scrollbar { width: 4px; }
  .grid-scroll::-webkit-scrollbar-track { background: #1a1a1a; border-radius: 2px; }
  .grid-scroll::-webkit-scrollbar-thumb { background: #00e5ff; border-radius: 2px; }
  .grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; }
  .grid button { background: #1a1a1a; border: 1px solid #222; color: #aaa; padding: 10px 3px; border-radius: 8px; font-size: 9px; font-weight: 600; transition: 0.15s; line-height: 1.2; }
  .grid button.active { background: #00e5ff22; border-color: #00e5ff; color: #00e5ff; box-shadow: 0 0 8px #00e5ff33; }
  .grid button:active { transform: scale(0.94); }
  .output-row { display: flex; gap: 8px; }
  .output-row button { flex: 1; background: #1a1a1a; border: 1px solid #222; color: #aaa; padding: 11px; border-radius: 8px; font-size: 12px; font-weight: 500; }
  .output-row button.active { background: #00e5ff22; border-color: #00e5ff; color: #00e5ff; }
  .song { display: flex; align-items: center; gap: 8px; padding: 10px 0; border-bottom: 1px solid #151515; }
  .song:last-child { border-bottom: none; }
  .song .name { flex: 1; font-size: 12px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .song button { background: #1a1a1a; border: 1px solid #222; color: #fff; padding: 6px 10px; border-radius: 6px; font-size: 11px; }
  .song button.sync { color: #00e5ff; border-color: #00e5ff55; }
  .footer { text-align: center; font-size: 10px; color: #444; padding: 14px 0 6px; }
  .toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%) translateY(100px); background: #00e5ff; color: #000; padding: 10px 20px; border-radius: 20px; font-size: 12px; font-weight: 600; transition: 0.3s; z-index: 999; }
  .toast.show { transform: translateX(-50%) translateY(0); }
</style>
</head>
<body>

<div class="header">
  <h1>SOUMYA GADGET</h1>
  <div class="ip" id="ip-label">192.168.4.1</div>
</div>

<div class="card">
  <h3>Now Playing</h3>
  <div class="player">
    <div class="cover">&#9835;</div>
    <div class="track">
      <div class="title" id="np-title">Kesariya</div>
      <div class="time" id="np-time">0:42 / 4:05</div>
      <div class="controls">
        <button onclick="musicCmd('prev')">&#9198;</button>
        <button class="play" onclick="musicCmd('toggle')" id="play-btn">&#9208;</button>
        <button onclick="musicCmd('next')">&#9197;</button>
      </div>
    </div>
  </div>
</div>

<div class="card">
  <h3>Color</h3>
  <div class="wheel-wrap"><canvas id="wheel" width="170" height="170"></canvas></div>
</div>

<div class="card">
  <h3>Settings</h3>
  <div class="slider-row">
    <label>Brightness</label>
    <input type="range" id="bright" min="0" max="255" value="150" oninput="setBright(this.value)">
    <span class="val" id="bright-val">150</span>
  </div>
  <div class="num-row">
    <label>LED Count</label>
    <div class="num-input-group">
      <button onclick="adjustCount(-1)">&minus;</button>
      <input type="number" id="count-input" min="1" max="144" value="60" onchange="setCount(this.value)" onkeypress="if(event.key==='Enter') this.blur();">
      <button onclick="adjustCount(1)">+</button>
    </div>
  </div>
</div>

<div class="card">
  <h3>Static Effects <span class="badge">16</span></h3>
  <div class="grid-scroll">
    <div class="grid" id="static-effects">
      <button onclick="setEffect(0, this)">Solid</button>
      <button onclick="setEffect(1, this)" class="active">Rainbow</button>
      <button onclick="setEffect(2, this)">Breathe</button>
      <button onclick="setEffect(3, this)">Chase</button>
      <button onclick="setEffect(4, this)">Color Wipe</button>
      <button onclick="setEffect(5, this)">Theater</button>
      <button onclick="setEffect(6, this)">Comet</button>
      <button onclick="setEffect(7, this)">Meteor</button>
      <button onclick="setEffect(8, this)">Pulse Wave</button>
      <button onclick="setEffect(9, this)">Twinkle</button>
      <button onclick="setEffect(10, this)">Fire</button>
      <button onclick="setEffect(11, this)">Sparkle</button>
      <button onclick="setEffect(12, this)">Scanner</button>
      <button onclick="setEffect(13, this)">Strobe</button>
      <button onclick="setEffect(14, this)">Two-Color</button>
      <button onclick="setEffect(15, this)">Gradient</button>
    </div>
  </div>
</div>

<div class="card">
  <h3>Music Sync <span class="badge">12</span></h3>
  <div class="grid-scroll">
    <div class="grid" id="music-effects">
      <button onclick="setEffect(16, this)">Music Bar</button>
      <button onclick="setEffect(17, this)">Music VU</button>
      <button onclick="setEffect(18, this)">Beat Pulse</button>
      <button onclick="setEffect(19, this)">Spectrum</button>
      <button onclick="setEffect(20, this)">Bass Pulse</button>
      <button onclick="setEffect(21, this)">Treble</button>
      <button onclick="setEffect(22, this)">VU Mirror</button>
      <button onclick="setEffect(23, this)">Wave Form</button>
      <button onclick="setEffect(24, this)">Freq Bars</button>
      <button onclick="setEffect(25, this)">Center Pulse</button>
      <button onclick="setEffect(26, this)">Beat Ripple</button>
      <button onclick="setEffect(27, this)">Peak Hold</button>
    </div>
  </div>
</div>

<div class="card">
  <h3>Audio Output</h3>
  <div class="output-row">
    <button id="out-wired" onclick="setOutput(0)">&#128268; Wired</button>
    <button id="out-bt" class="active" onclick="setOutput(1)">&#128225; Bluetooth</button>
  </div>
</div>

<div class="card">
  <h3>Songs</h3>
  <div id="song-list">
    <div class="song"><div class="name">Tum Hi Ho</div><button onclick="playIdx(0)">&#9654;</button><button class="sync" onclick="syncIdx(0)">SYNC</button></div>
    <div class="song"><div class="name">Kesariya</div><button onclick="playIdx(1)">&#9654;</button><button class="sync" onclick="syncIdx(1)">SYNC</button></div>
    <div class="song"><div class="name">Apna Bana Le</div><button onclick="playIdx(2)">&#9654;</button><button class="sync" onclick="syncIdx(2)">SYNC</button></div>
  </div>
</div>

<div class="footer">SOUMYA Gadget v9.0 &bull; Port 80</div>
<div class="toast" id="toast">OK</div>

<script>
function toast(msg) {
  var t = document.getElementById('toast');
  t.innerText = msg; t.classList.add('show');
  clearTimeout(window._tt);
  window._tt = setTimeout(function(){ t.classList.remove('show'); }, 1200);
}
function api(url) { fetch(url).catch(function(){}); }

function musicCmd(action) {
  if (action === 'toggle') {
    var btn = document.getElementById('play-btn');
    btn.innerText = btn.innerText === '\u23F8' ? '\u25B6' : '\u23F8';
    toast('Toggle playback');
  } else toast(action === 'next' ? 'Next song' : 'Previous song');
  api('/music?action=' + action);
}
function setOutput(n) {
  document.getElementById('out-wired').classList.toggle('active', n === 0);
  document.getElementById('out-bt').classList.toggle('active', n === 1);
  toast(n === 0 ? 'Wired output' : 'Bluetooth output');
  api('/music?action=bt&on=' + n);
}
function setBright(v) { document.getElementById('bright-val').innerText = v; api('/led?brightness=' + v); }
function setCount(v) {
  var n = parseInt(v);
  if (isNaN(n)) n = 60;
  if (n < 1) n = 1; if (n > 144) n = 144;
  document.getElementById('count-input').value = n;
  toast('LED Count: ' + n); api('/led?count=' + n);
}
function adjustCount(d) {
  var inp = document.getElementById('count-input');
  var n = (parseInt(inp.value) || 60) + d;
  if (n < 1) n = 1; if (n > 144) n = 144;
  inp.value = n; toast('LED Count: ' + n); api('/led?count=' + n);
}
function setEffect(n, btn) {
  var all = document.querySelectorAll('.grid button');
  for (var i = 0; i < all.length; i++) all[i].classList.remove('active');
  btn.classList.add('active');
  toast(btn.innerText); api('/led?effect=' + n);
}

var wheel = document.getElementById('wheel');
var wctx = wheel.getContext('2d');
var W = 170, cx = W/2, cy = W/2, R = W/2 - 6;
var wheelDrawn = false;
function hsl2rgb(h, s, l) {
  var r, g, b;
  if (s === 0) { r = g = b = l; }
  else {
    var h2r = function(p, q, t) {
      if (t < 0) t += 1; if (t > 1) t -= 1;
      if (t < 1/6) return p + (q-p)*6*t;
      if (t < 1/2) return q;
      if (t < 2/3) return p + (q-p)*(2/3-t)*6;
      return p;
    };
    var q = l < 0.5 ? l*(1+s) : l+s-l*s;
    var p = 2*l - q;
    r = h2r(p, q, h+1/3); g = h2r(p, q, h); b = h2r(p, q, h-1/3);
  }
  return [Math.round(r*255), Math.round(g*255), Math.round(b*255)];
}
function drawWheel() {
  if (wheelDrawn) return;
  var img = wctx.createImageData(W, W);
  for (var y = 0; y < W; y++) {
    for (var x = 0; x < W; x++) {
      var dx = x - cx, dy = y - cy;
      var dist = Math.sqrt(dx*dx + dy*dy);
      var i = (y*W + x) * 4;
      if (dist <= R) {
        var ang = Math.atan2(dy, dx) * 180 / Math.PI;
        if (ang < 0) ang += 360;
        var rgb = hsl2rgb(ang/360, dist/R, 0.5);
        img.data[i] = rgb[0]; img.data[i+1] = rgb[1]; img.data[i+2] = rgb[2]; img.data[i+3] = 255;
      } else img.data[i+3] = 0;
    }
  }
  wctx.putImageData(img, 0, 0);
  wctx.beginPath(); wctx.arc(cx, cy, R * 0.15, 0, Math.PI * 2);
  wctx.fillStyle = '#fff'; wctx.fill(); wheelDrawn = true;
}
var dragging = false;
function pickColor(e) {
  var rect = wheel.getBoundingClientRect();
  var scale = W / rect.width;
  var x = (e.clientX - rect.left) * scale;
  var y = (e.clientY - rect.top) * scale;
  var dx = x - cx, dy = y - cy;
  var dist = Math.sqrt(dx*dx + dy*dy);
  if (dist > R) return;
  var ang = Math.atan2(dy, dx) * 180 / Math.PI;
  if (ang < 0) ang += 360;
  api('/led?hue=' + Math.round((ang/360) * 255));
}
wheel.addEventListener('mousedown', function(e){ dragging = true; pickColor(e); });
wheel.addEventListener('mousemove', function(e){ if (dragging) pickColor(e); });
window.addEventListener('mouseup', function(){ dragging = false; });
wheel.addEventListener('touchstart', function(e){ e.preventDefault(); pickColor(e.touches[0]); });
wheel.addEventListener('touchmove', function(e){ e.preventDefault(); pickColor(e.touches[0]); });
drawWheel();

function playIdx(i) { api('/music?action=play&idx=' + i); toast('Playing...'); }
function syncIdx(i) { playIdx(i); setEffect(16, document.querySelector('#music-effects button')); }
</script>
</body>
</html>
)rawliteral";

#endif
