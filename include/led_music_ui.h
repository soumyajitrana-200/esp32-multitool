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
  body { background: #050505; color: #fff; font-family: 'Segoe UI', -apple-system, sans-serif; padding: 12px; min-height: 100vh; }
  .header { display: flex; justify-content: space-between; align-items: center; padding: 8px 0 16px; border-bottom: 1px solid #00e5ff33; margin-bottom: 16px; }
  .header h1 { font-size: 16px; color: #00e5ff; letter-spacing: 2px; text-shadow: 0 0 8px #00e5ff66; }
  .header .ip { font-size: 11px; color: #666; }
  .card { background: #0d0d0d; border: 1px solid #1a1a1a; border-radius: 14px; padding: 16px; margin-bottom: 14px; }
  .card h3 { font-size: 12px; color: #00e5ff; text-transform: uppercase; letter-spacing: 1.5px; margin-bottom: 12px; font-weight: 600; }
  .player { display: flex; gap: 14px; align-items: center; }
  .cover { width: 64px; height: 64px; border-radius: 10px; background: linear-gradient(135deg, #00e5ff22, #ff00aa22); border: 1px solid #00e5ff44; display: flex; align-items: center; justify-content: center; font-size: 24px; flex-shrink: 0; }
  .track { flex: 1; min-width: 0; }
  .track .title { font-size: 15px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; margin-bottom: 4px; }
  .track .time { font-size: 11px; color: #888; margin-bottom: 8px; }
  .controls { display: flex; gap: 8px; }
  .controls button { background: #1a1a1a; border: none; color: #fff; width: 36px; height: 36px; border-radius: 8px; font-size: 14px; transition: 0.2s; }
  .controls button:active { background: #00e5ff; color: #000; }
  .wheel-wrap { display: flex; justify-content: center; position: relative; }
  canvas#wheel { width: 180px; height: 180px; border-radius: 50%; box-shadow: 0 0 20px #00e5ff33; cursor: pointer; }
  .slider-row { display: flex; align-items: center; gap: 12px; margin: 10px 0; }
  .slider-row label { font-size: 12px; color: #999; min-width: 70px; }
  .slider-row .val { font-size: 12px; color: #00e5ff; min-width: 40px; text-align: right; font-weight: 600; }
  input[type=range] { -webkit-appearance: none; flex: 1; background: #1a1a1a; height: 6px; border-radius: 3px; outline: none; }
  input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 18px; height: 18px; background: #00e5ff; border-radius: 50%; box-shadow: 0 0 10px #00e5ff; cursor: pointer; }
  .effect-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; }
  .effect-grid button { background: #1a1a1a; border: 1px solid #222; color: #aaa; padding: 10px 4px; border-radius: 8px; font-size: 11px; font-weight: 500; transition: 0.2s; }
  .effect-grid button.active { background: #00e5ff22; border-color: #00e5ff; color: #00e5ff; box-shadow: 0 0 10px #00e5ff44; }
  .song { display: flex; align-items: center; gap: 10px; padding: 10px 0; border-bottom: 1px solid #151515; }
  .song:last-child { border-bottom: none; }
  .song .name { flex: 1; font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .song button { background: #1a1a1a; border: 1px solid #222; color: #fff; padding: 6px 10px; border-radius: 6px; font-size: 12px; }
  .song button.sync { color: #00e5ff; border-color: #00e5ff55; }
  .output-row { display: flex; gap: 8px; }
  .output-row button { flex: 1; background: #1a1a1a; border: 1px solid #222; color: #aaa; padding: 10px; border-radius: 8px; font-size: 12px; }
  .output-row button.active { background: #00e5ff22; border-color: #00e5ff; color: #00e5ff; }
  .footer { text-align: center; font-size: 10px; color: #444; padding: 16px 0 8px; }
  .toast { position: fixed; bottom: 24px; left: 50%; transform: translateX(-50%) translateY(100px); background: #00e5ff; color: #000; padding: 10px 20px; border-radius: 20px; font-size: 12px; font-weight: 600; transition: 0.3s; z-index: 999; }
  .toast.show { transform: translateX(-50%) translateY(0); }
</style>
</head>
<body>

<div class="header">
  <h1>SOUMYA GADGET</h1>
  <div class="ip">192.168.4.1</div>
</div>

<div class="card">
  <h3>Now Playing</h3>
  <div class="player">
    <div class="cover">♪</div>
    <div class="track">
      <div class="title" id="np-title">No song</div>
      <div class="time" id="np-time">0:00 / 0:00</div>
      <div class="controls">
        <button onclick="musicCmd('prev')">⏮</button>
        <button onclick="musicCmd('toggle')" id="play-btn">▶</button>
        <button onclick="musicCmd('next')">⏭</button>
      </div>
    </div>
  </div>
</div>

<div class="card">
  <h3>Color</h3>
  <div class="wheel-wrap">
    <canvas id="wheel" width="180" height="180"></canvas>
  </div>
</div>

<div class="card">
  <h3>Settings</h3>
  <div class="slider-row">
    <label>Brightness</label>
    <input type="range" id="bright" min="0" max="255" value="150" oninput="setBright(this.value)">
    <span class="val" id="bright-val">150</span>
  </div>
  <div class="slider-row">
    <label>LED Count</label>
    <input type="range" id="count" min="1" max="144" value="60" oninput="setCount(this.value)">
    <span class="val" id="count-val">60</span>
  </div>
</div>

<div class="card">
  <h3>Effects</h3>
  <div class="effect-grid" id="static-effects">
    <button onclick="setEffect(0, this)">Solid</button>
    <button onclick="setEffect(1, this)" class="active">Rainbow</button>
    <button onclick="setEffect(2, this)">Breathe</button>
    <button onclick="setEffect(3, this)">Chase</button>
  </div>
</div>

<div class="card">
  <h3>Music Sync</h3>
  <div class="effect-grid" id="music-effects">
    <button onclick="setEffect(4, this)">Music Bar</button>
    <button onclick="setEffect(5, this)">Music VU</button>
    <button onclick="setEffect(6, this)">Beat Pulse</button>
    <button onclick="setEffect(7, this)">Spectrum</button>
    <button onclick="setEffect(8, this)">Bass</button>
    <button onclick="setEffect(9, this)">Treble</button>
    <button onclick="setEffect(10, this)">VU Mirror</button>
    <button onclick="setEffect(11, this)">Wave</button>
  </div>
</div>

<div class="card">
  <h3>Output</h3>
  <div class="output-row">
    <button id="out-wired" class="active" onclick="setOutput(0)">🔌 Wired</button>
    <button id="out-bt" onclick="setOutput(1)">📡 Bluetooth</button>
  </div>
</div>

<div class="card">
  <h3>Songs</h3>
  <div id="song-list"><div style="text-align:center;color:#555;font-size:12px;padding:12px;">Loading...</div></div>
</div>

<div class="footer">SOUMYA Gadget v9.0 • ESP32</div>
<div class="toast" id="toast">OK</div>

<script>
let currentEffect = 1;
let playing = false;
let isDraggingWheel = false;

function toast(msg) {
  const t = document.getElementById('toast');
  t.innerText = msg;
  t.classList.add('show');
  clearTimeout(window._toastT);
  window._toastT = setTimeout(() => t.classList.remove('show'), 1200);
}
function api(url) { return fetch(url).then(r => r.text()).catch(e => console.error(e)); }
function musicCmd(action) {
  if (action === 'toggle') {
    playing = !playing;
    document.getElementById('play-btn').innerText = playing ? '⏸' : '▶';
  }
  api('/music?action=' + action);
  toast(action);
}
function setOutput(n) {
  document.getElementById('out-wired').classList.toggle('active', n === 0);
  document.getElementById('out-bt').classList.toggle('active', n === 1);
  api('/music?action=bt&on=' + n);
  toast(n === 0 ? 'Wired output' : 'Bluetooth output');
}
function setBright(v) { document.getElementById('bright-val').innerText = v; api('/led?brightness=' + v); }
function setCount(v) { document.getElementById('count-val').innerText = v; api('/led?count=' + v); }
function setEffect(n, btn) {
  currentEffect = n;
  const parent = btn.parentElement;
  parent.querySelectorAll('button').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  api('/led?effect=' + n);
}

const wheel = document.getElementById('wheel');
const wctx = wheel.getContext('2d');
const W = 180, cx = W / 2, cy = W / 2, R = W / 2 - 6;
let wheelDrawn = false;
function drawWheel() {
  if (wheelDrawn) return;
  const img = wctx.createImageData(W, W);
  for (let y = 0; y < W; y++) {
    for (let x = 0; x < W; x++) {
      const dx = x - cx, dy = y - cy;
      const dist = Math.sqrt(dx * dx + dy * dy);
      const i = (y * W + x) * 4;
      if (dist <= R) {
        let ang = Math.atan2(dy, dx) * 180 / Math.PI;
        if (ang < 0) ang += 360;
        const [r, g, b] = hslToRgb(ang / 360, dist / R, 0.5);
        img.data[i] = r; img.data[i + 1] = g; img.data[i + 2] = b; img.data[i + 3] = 255;
      } else { img.data[i + 3] = 0; }
    }
  }
  wctx.putImageData(img, 0, 0);
  wctx.beginPath();
  wctx.arc(cx, cy, R * 0.15, 0, Math.PI * 2);
  wctx.fillStyle = '#fff';
  wctx.fill();
  wheelDrawn = true;
}
function hslToRgb(h, s, l) {
  let r, g, b;
  if (s === 0) { r = g = b = l; }
  else {
    const h2r = (p, q, t) => {
      if (t < 0) t += 1; if (t > 1) t -= 1;
      if (t < 1/6) return p + (q - p) * 6 * t;
      if (t < 1/2) return q;
      if (t < 2/3) return p + (q - p) * (2/3 - t) * 6;
      return p;
    };
    const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const p = 2 * l - q;
    r = h2r(p, q, h + 1/3); g = h2r(p, q, h); b = h2r(p, q, h - 1/3);
  }
  return [Math.round(r * 255), Math.round(g * 255), Math.round(b * 255)];
}
function pickColor(e) {
  const rect = wheel.getBoundingClientRect();
  const x = e.clientX - rect.left;
  const y = e.clientY - rect.top;
  const dx = x - cx, dy = y - cy;
  const dist = Math.sqrt(dx * dx + dy * dy);
  if (dist > R) return;
  let ang = Math.atan2(dy, dx) * 180 / Math.PI;
  if (ang < 0) ang += 360;
  const hueInt = Math.round((ang / 360) * 255);
  api('/led?hue=' + hueInt);
}
wheel.addEventListener('mousedown', e => { isDraggingWheel = true; pickColor(e); });
wheel.addEventListener('mousemove', e => { if (isDraggingWheel) pickColor(e); });
window.addEventListener('mouseup', () => isDraggingWheel = false);
wheel.addEventListener('touchstart', e => { e.preventDefault(); pickColor(e.touches[0]); });
wheel.addEventListener('touchmove', e => { e.preventDefault(); pickColor(e.touches[0]); });
drawWheel();

function loadSongs() {
  fetch('/music/list').then(r => r.json()).then(data => {
    const el = document.getElementById('song-list');
    if (!data.songs || data.songs.length === 0) {
      el.innerHTML = '<div style="text-align:center;color:#555;font-size:12px;padding:12px;">No songs found</div>';
      return;
    }
    let html = '';
    data.songs.forEach(s => {
      html += '<div class="song">';
      html += '<div class="name">' + s.name + '</div>';
      html += '<button onclick="playIdx(' + s.idx + ')">▶</button>';
      html += '<button class="sync" onclick="syncIdx(' + s.idx + ')">SYNC</button>';
      html += '</div>';
    });
    el.innerHTML = html;
    if (data.current !== undefined && data.songs[data.current]) {
      document.getElementById('np-title').innerText = data.songs[data.current].name;
    }
    playing = data.playing;
    document.getElementById('play-btn').innerText = playing ? '⏸' : '▶';
    document.getElementById('out-wired').classList.toggle('active', !data.bt);
    document.getElementById('out-bt').classList.toggle('active', data.bt);
  }).catch(e => {
    document.getElementById('song-list').innerHTML = '<div style="text-align:center;color:#555;font-size:12px;padding:12px;">SD card not found</div>';
  });
}
function playIdx(i) {
  api('/music?action=play&idx=' + i);
  playing = true;
  document.getElementById('play-btn').innerText = '⏸';
  toast('Playing...');
  setTimeout(loadSongs, 500);
}
function syncIdx(i) {
  playIdx(i);
  setEffect(4, document.querySelector('#music-effects button'));
}
setInterval(() => {
  fetch('/music/list').then(r => r.json()).then(data => {
    if (data.current !== undefined && data.songs[data.current]) {
      document.getElementById('np-title').innerText = data.songs[data.current].name;
    }
    playing = data.playing;
    document.getElementById('play-btn').innerText = playing ? '⏸' : '▶';
  }).catch(() => {});
}, 3000);
loadSongs();
</script>
</body>
</html>
)rawliteral";

#endif
