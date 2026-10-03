#pragma once
// SOUMYA Gadget — LED + Music Web UI
// Served at http://192.168.4.1/

const char LED_MUSIC_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>SOUMYA Gadget</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{background:#050505;color:#fff;font-family:-apple-system,'Segoe UI',sans-serif;padding:12px;min-height:100vh}
.header{display:flex;justify-content:space-between;align-items:center;padding:8px 0 16px;border-bottom:1px solid #00e5ff33;margin-bottom:14px}
.header h1{font-size:15px;color:#00e5ff;letter-spacing:2px;text-shadow:0 0 10px #00e5ff66;font-weight:700}
.header .ip{font-size:11px;color:#666;font-family:monospace}
.card{background:#0d0d0d;border:1px solid #1a1a1a;border-radius:14px;padding:14px;margin-bottom:12px}
.card h3{font-size:11px;color:#00e5ff;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:12px;font-weight:600;display:flex;justify-content:space-between;align-items:center}
.card h3 .badge{background:#00e5ff22;color:#00e5ff;font-size:10px;padding:2px 8px;border-radius:10px;font-weight:700}
.player{display:flex;gap:12px;align-items:center}
.cover{width:60px;height:60px;border-radius:10px;background:linear-gradient(135deg,#00e5ff22,#ff00aa22);border:1px solid #00e5ff44;display:flex;align-items:center;justify-content:center;font-size:22px}
.track{flex:1;min-width:0}
.track .title{font-size:14px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-bottom:8px}
.controls{display:flex;gap:6px}
.controls button{background:#1a1a1a;border:none;color:#fff;width:34px;height:34px;border-radius:8px;font-size:13px}
.controls button:active{background:#00e5ff;color:#000}
.controls button.play{background:#00e5ff;color:#000}
.wheel-wrap{display:flex;justify-content:center;padding:10px 0}
canvas#wheel{width:170px;height:170px;border-radius:50%;box-shadow:0 0 20px #00e5ff33;cursor:pointer;touch-action:none}
.slider-row{display:flex;align-items:center;gap:10px;margin:10px 0}
.slider-row label{font-size:11px;color:#999;min-width:65px}
.slider-row .val{font-size:12px;color:#00e5ff;min-width:35px;text-align:right;font-weight:600;font-family:monospace}
input[type=range]{-webkit-appearance:none;flex:1;background:#1a1a1a;height:5px;border-radius:3px;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:18px;height:18px;background:#00e5ff;border-radius:50%}
.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
.grid button{background:#1a1a1a;border:1px solid #222;color:#aaa;padding:10px 3px;border-radius:8px;font-size:9px;font-weight:600}
.grid button.active{background:#00e5ff22;border-color:#00e5ff;color:#00e5ff}
.grid button:active{transform:scale(0.94)}
.song{display:flex;align-items:center;gap:8px;padding:10px 0;border-bottom:1px solid #151515}
.song:last-child{border-bottom:none}
.song .name{flex:1;font-size:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.song button{background:#1a1a1a;border:1px solid #222;color:#fff;padding:6px 10px;border-radius:6px;font-size:11px}
.footer{text-align:center;font-size:10px;color:#444;padding:14px 0 6px}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(100px);background:#00e5ff;color:#000;padding:10px 20px;border-radius:20px;font-size:12px;font-weight:600;transition:.3s;z-index:999}
.toast.show{transform:translateX(-50%) translateY(0)}
.toast.err{background:#ff3366;color:#fff}
.fwbtn{background:#ff3366;color:#fff;padding:12px;border:none;border-radius:8px;width:100%;margin-top:10px;font-weight:700;font-size:13px}
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
<div class="cover">&#9835;</div>
<div class="track">
<div class="title" id="np-title">—</div>
<div class="controls">
<button onclick="musicCmd('prev')">&#9198;</button>
<button class="play" onclick="musicCmd('toggle')" id="play-btn">&#9654;</button>
<button onclick="musicCmd('next')">&#9197;</button>
<button onclick="musicCmd('stop')">&#9632;</button>
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
<input type="range" id="bright" min="0" max="255" value="150">
<span class="val" id="bright-val">150</span>
</div>
<div class="slider-row">
<label>LED Count</label>
<input type="range" id="count" min="1" max="144" value="60">
<span class="val" id="count-val">60</span>
</div>
</div>

<div class="card">
<h3>Static Effects <span class="badge">16</span></h3>
<div class="grid" id="fx-static">
<button data-fx="0">Solid</button><button data-fx="1" class="active">Rainbow</button>
<button data-fx="2">Breathe</button><button data-fx="3">Chase</button>
<button data-fx="4">Comet</button><button data-fx="5">Pulse</button>
<button data-fx="6">Glow</button><button data-fx="7">Wipe</button>
<button data-fx="8">Twinkle</button><button data-fx="9">Strobe</button>
<button data-fx="10">Two-color</button><button data-fx="11">Gradient</button>
<button data-fx="12">Scanner</button><button data-fx="13">Fire</button>
<button data-fx="14">Sparkle</button><button data-fx="15">Theater</button>
</div>
</div>

<div class="card">
<h3>Music Sync <span class="badge">12</span></h3>
<div class="grid" id="fx-music">
<button data-fx="16">Music Bar</button><button data-fx="17">Center</button>
<button data-fx="18">Beat</button><button data-fx="19">Spectrum</button>
<button data-fx="20">Bass</button><button data-fx="21">Treble</button>
<button data-fx="22">VU Mirror</button><button data-fx="23">Wave</button>
<button data-fx="24">Freq Bars</button><button data-fx="25">Pulse C</button>
<button data-fx="26">Ripple</button><button data-fx="27">Peak</button>
</div>
</div>

<div class="card">
<h3>Songs</h3>
<div id="song-list"><div style="color:#555;font-style:italic;font-size:12px;text-align:center;padding:18px">Loading…</div></div>
</div>

<div class="card">
<h3>Firmware Update</h3>
<input type="file" id="fw-file" accept=".bin" style="color:#fff;background:#1a1a1a;padding:10px;border-radius:8px;width:100%;font-size:12px">
<button class="fwbtn" onclick="uploadFirmware()">Upload &amp; Reboot</button>
<div id="fw-progress" style="color:#00e5ff;font-size:12px;margin-top:8px;text-align:center"></div>
</div>

<div class="footer">SOUMYA Gadget v9.1</div>
<div class="toast" id="toast">OK</div>

<script>
var $=function(i){return document.getElementById(i)};
var currentFx=1,isPlaying=false,playingIdx=-1;

function toast(m,e){
var t=$('toast');t.textContent=m;
t.className='toast'+(e?' err':'');
t.classList.add('show');
clearTimeout(t._t);
t._t=setTimeout(function(){t.classList.remove('show')},1500);
}

function api(p){
return fetch(p).then(function(r){
if(!r.ok)throw new Error(r.status);
return r;
}).catch(function(e){toast('Device offline',1);throw e;});
}

function apiJson(p){return api(p).then(function(r){return r.json()});}

function musicCmd(a){
if(a==='toggle'){
apiJson('/music/list').then(function(st){
if(st.playing)return api('/music?action=stop').then(refresh);
var idx=st.current>=0?st.current:0;
if(st.songs.length===0){toast('No songs',1);return;}
return api('/music?action=play&idx='+idx).then(refresh);
});
return;
}
var u=a==='play'?'/music?action=play&idx='+(window._playIdx||0):'/music?action='+a;
api(u).then(function(){setTimeout(refresh,300)});
}

document.querySelectorAll('.grid button').forEach(function(b){
b.addEventListener('click',function(){
document.querySelectorAll('.grid button').forEach(function(x){x.classList.remove('active')});
b.classList.add('active');
currentFx=parseInt(b.dataset.fx);
api('/led?effect='+currentFx).then(function(){toast(b.textContent)});
});
});

var brightT=null;
$('bright').addEventListener('input',function(e){
$('bright-val').textContent=e.target.value;
clearTimeout(brightT);
brightT=setTimeout(function(){api('/led?brightness='+e.target.value)},80);
});

var countT=null;
$('count').addEventListener('input',function(e){
$('count-val').textContent=e.target.value;
clearTimeout(countT);
countT=setTimeout(function(){api('/led?count='+e.target.value)},120);
});

var wheel=$('wheel'),wctx=wheel.getContext('2d');
var W=170,cx=W/2,cy=W/2,R=W/2-6;

function hsl2rgb(h,s,l){
if(s===0){var v=Math.round(l*255);return[v,v,v];}
function h2r(p,q,t){if(t<0)t+=1;if(t>1)t-=1;if(t<1/6)return p+(q-p)*6*t;if(t<1/2)return q;if(t<2/3)return p+(q-p)*(2/3-t)*6;return p;}
var q=l<0.5?l*(1+s):l+s-l*s,p=2*l-q;
return[Math.round(h2r(p,q,h+1/3)*255),Math.round(h2r(p,q,h)*255),Math.round(h2r(p,q,h-1/3)*255)];
}

(function(){
var img=wctx.createImageData(W,W);
for(var y=0;y<W;y++)for(var x=0;x<W;x++){
var dx=x-cx,dy=y-cy,d=Math.sqrt(dx*dx+dy*dy);
var i=(y*W+x)*4;
if(d<=R){
var a=Math.atan2(dy,dx)*180/Math.PI;if(a<0)a+=360;
var rgb=hsl2rgb(a/360,d/R,0.5);
img.data[i]=rgb[0];img.data[i+1]=rgb[1];img.data[i+2]=rgb[2];img.data[i+3]=255;
}
}
wctx.putImageData(img,0,0);
wctx.beginPath();wctx.arc(cx,cy,R*0.15,0,Math.PI*2);
wctx.fillStyle='#fff';wctx.fill();
})();

var dragging=false;
function pick(e){
var r=wheel.getBoundingClientRect();
var sc=W/r.width;
var x=(e.clientX-r.left)*sc,y=(e.clientY-r.top)*sc;
var dx=x-cx,dy=y-cy,d=Math.sqrt(dx*dx+dy*dy);
if(d>R)return;
var a=Math.atan2(dy,dx)*180/Math.PI;if(a<0)a+=360;
api('/led?hue='+Math.round(a/360*255));
}
wheel.addEventListener('pointerdown',function(e){dragging=true;pick(e)});
wheel.addEventListener('pointermove',function(e){if(dragging)pick(e)});
window.addEventListener('pointerup',function(){dragging=false});

function esc(s){return s.replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}

function loadSongs(){
return apiJson('/music/list').then(function(j){
var list=$('song-list');
if(!j.songs.length){
list.innerHTML='<div style="color:#555;font-style:italic;font-size:12px;text-align:center;padding:18px">No songs. Upload via <b>/files</b></div>';
return;
}
list.innerHTML=j.songs.map(function(s){
return '<div class="song"><div class="name">'+esc(s.name)+'</div>'+
'<button onclick="playIdx('+s.idx+')">&#9654;</button></div>';
}).join('');
playingIdx=j.current;
$('np-title').textContent=j.current>=0?j.songs.find(function(x){return x.idx===j.current}).name:'—';
isPlaying=j.playing;
$('play-btn').textContent=isPlaying?'&#10074;&#10074;':'&#9654;';
currentFx=j.effect;
$('bright').value=j.bright;$('bright-val').textContent=j.bright;
$('count').value=j.count;$('count-val').textContent=j.count;
document.querySelectorAll('.grid button').forEach(function(b){
b.classList.toggle('active',+b.dataset.fx===currentFx);
});
});
}

function playIdx(i){window._playIdx=i;api('/music?action=play&idx='+i).then(function(){setTimeout(loadSongs,300)})}
function refresh(){return loadSongs().catch(function(){})}

function uploadFirmware(){
var f=$('fw-file').files[0];
if(!f){alert('Select .bin file first');return;}
if(!confirm('Update firmware? ESP32 will reboot.'))return;
var xhr=new XMLHttpRequest();
xhr.open('POST','/firmware/upload');
xhr.upload.onprogress=function(e){
var pct=Math.round(e.loaded/e.total*100);
$('fw-progress').textContent='Uploading: '+pct+'%';
};
xhr.onload=function(){
$('fw-progress').textContent='Uploaded! Rebooting...';
fetch('/firmware/reboot',{method:'POST'}).catch(function(){});
setTimeout(function(){
document.body.innerHTML='<div style="text-align:center;padding:80px 20px;color:#00e5ff;font-family:sans-serif"><h2>Firmware flashing...</h2><p style="color:#888;margin-top:12px">Reconnect to WiFi and open 192.168.4.1 in ~20 sec</p></div>';
},500);
};
xhr.onerror=function(){alert('Upload failed')};
var fd=new FormData();fd.append('file',f,'firmware.bin');xhr.send(fd);
}

setInterval(refresh,3000);
refresh();
</script>
</body>
</html>
)rawliteral";
