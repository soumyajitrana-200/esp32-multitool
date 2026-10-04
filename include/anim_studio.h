#pragma once
// OLED Studio — Browser tool for video -> OLED animation
// Served from ESP32 at http://192.168.4.1/anim

const char ANIM_STUDIO_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>OLED Studio</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{background:#050505;color:#fff;font-family:-apple-system,'Segoe UI',sans-serif;padding:12px;min-height:100vh}
.header{display:flex;justify-content:space-between;align-items:center;padding:8px 0 16px;border-bottom:1px solid #00e5ff33;margin-bottom:14px}
.header h1{font-size:15px;color:#00e5ff;letter-spacing:2px;font-weight:700}
.header .ip{font-size:11px;color:#666;font-family:monospace}
.card{background:#0d0d0d;border:1px solid #1a1a1a;border-radius:14px;padding:14px;margin-bottom:12px}
.card h3{font-size:11px;color:#00e5ff;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:12px;font-weight:600}
.upload-zone{border:2px dashed #00e5ff44;border-radius:12px;padding:20px;text-align:center;position:relative;margin-bottom:12px}
.upload-zone input{position:absolute;inset:0;opacity:0;cursor:pointer}
.upload-zone .icon{font-size:28px;color:#00e5ff;margin-bottom:8px}
.upload-zone .label{font-size:13px;color:#00e5ff;font-weight:600}
.upload-zone .hint{font-size:11px;color:#666;margin-top:4px}
.preview-wrap{display:flex;justify-content:center;background:#000;padding:12px;border-radius:10px}
canvas#oled{width:320px;height:160px;image-rendering:pixelated;border:1px solid #00e5ff44;border-radius:4px}
.row{display:flex;align-items:center;gap:10px;margin:10px 0}
.row label{font-size:11px;color:#999;min-width:80px}
.row .val{font-size:12px;color:#00e5ff;min-width:40px;text-align:right;font-weight:600;font-family:monospace}
input[type=range]{-webkit-appearance:none;flex:1;background:#1a1a1a;height:5px;border-radius:3px;outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:18px;height:18px;background:#00e5ff;border-radius:50%}
.btn{display:block;width:100%;padding:12px;border:none;border-radius:10px;font-size:13px;font-weight:700;margin-top:10px;letter-spacing:1px}
.btn.primary{background:#00e5ff;color:#000}
.btn.danger{background:#ff3366;color:#fff}
.btn:disabled{opacity:0.4}
.status{text-align:center;font-size:11px;color:#666;margin-top:8px;font-family:monospace}
.progress{margin-top:12px;display:none}
.progress.show{display:block}
.bar{width:100%;height:8px;background:#1a1a1a;border-radius:4px;overflow:hidden;margin:6px 0}
.bar .fill{height:100%;background:linear-gradient(90deg,#00e5ff,#ff00aa);transition:width .2s;border-radius:4px}
.prog-label{font-size:11px;color:#00e5ff;text-align:right}
.info{font-size:11px;color:#888;text-align:center;margin-top:8px;line-height:1.6}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(100px);background:#00e5ff;color:#000;padding:10px 20px;border-radius:20px;font-size:12px;font-weight:600;transition:.3s;z-index:999}
.toast.show{transform:translateX(-50%) translateY(0)}
.toast.err{background:#ff3366;color:#fff}
</style>
</head>
<body>

<div class="header">
<h1>OLED STUDIO</h1>
<div class="ip" id="ip">192.168.4.1</div>
</div>

<div class="card">
<h3>1. Select Video</h3>
<div class="upload-zone">
<div class="icon">&#127916;</div>
<div class="label">Tap to select video</div>
<div class="hint">MP4, WebM, MOV &mdash; max 6 min</div>
<input type="file" id="vfile" accept="video/*">
</div>
<video id="vid" style="display:none" playsinline muted loop></video>
</div>

<div class="card" id="preview-card" style="display:none">
<h3>2. Preview</h3>
<div class="preview-wrap">
<canvas id="oled" width="128" height="64"></canvas>
</div>
<div class="info" id="vinfo">&mdash;</div>
</div>

<div class="card" id="settings-card" style="display:none">
<h3>3. Settings</h3>
<div class="row">
<label>Threshold</label>
<input type="range" id="thr" min="40" max="220" value="128">
<span class="val" id="thr-v">128</span>
</div>
<div class="row">
<label>FPS</label>
<input type="range" id="fps" min="8" max="20" value="15">
<span class="val" id="fps-v">15</span>
</div>
<div class="row">
<label>Duration</label>
<input type="range" id="dur" min="1" max="360" value="60">
<span class="val" id="dur-v">60s</span>
</div>
<div class="info" id="estimate">&mdash;</div>
</div>

<div class="card" id="upload-card" style="display:none">
<h3>4. Upload to ESP32</h3>
<button class="btn primary" id="btn-process">Process &amp; Upload</button>
<div class="progress" id="prog">
<div class="prog-label" id="prog-lbl">0%</div>
<div class="bar"><div class="fill" id="prog-fill" style="width:0%"></div></div>
<div class="status" id="prog-status">&mdash;</div>
</div>
</div>

<div class="toast" id="toast">OK</div>

<script>
var $=function(i){return document.getElementById(i)};
var video=$('vid');
var canvas=$('oled');
var ctx=canvas.getContext('2d');
var W=128, H=64;

function toast(m,e){
var t=$('toast');
t.textContent=m;
t.className='toast'+(e?' err':'');
t.classList.add('show');
clearTimeout(t._t);
t._t=setTimeout(function(){t.classList.remove('show')},2000);
}

// ---------------- File select ----------------
$('vfile').addEventListener('change',function(e){
if(!e.target.files.length)return;
var f=e.target.files[0];
video.src=URL.createObjectURL(f);
video.onloadedmetadata=function(){
$('vinfo').textContent=f.name+'  |  '+video.videoWidth+'x'+video.videoHeight+'  |  '+video.duration.toFixed(1)+'s';
$('preview-card').style.display='block';
$('settings-card').style.display='block';
$('upload-card').style.display='block';
// Auto-set duration slider to min(duration, 360)
$('dur').max=Math.max(1,Math.floor(video.duration));
$('dur').value=Math.min(Math.floor(video.duration),60);
$('dur-v').textContent=$('dur').value+'s';
updateEstimate();
video.currentTime=0;
video.play();
};
});

// ---------------- Live preview loop ----------------
function drawPreview(){
if(video.readyState>=2){
ctx.drawImage(video,0,0,W,H);
dither(ctx,parseInt($('thr').value));
}
}
setInterval(function(){ if(!video.paused) drawPreview(); }, 66);

// Redraw on threshold change
['thr','fps','dur'].forEach(function(id){
$(id).addEventListener('input',function(){
if(id==='thr'){$('thr-v').textContent=this.value; drawPreview();}
if(id==='fps'){$('fps-v').textContent=this.value; updateEstimate();}
if(id==='dur'){$('dur-v').textContent=this.value+'s'; updateEstimate();}
});
});

function updateEstimate(){
var sec=parseInt($('dur').value);
var fps=parseInt($('fps').value);
var frames=sec*fps;
var bytes=frames*1024+16;
$('estimate').textContent=frames+' frames  |  '+(bytes/1048576).toFixed(2)+' MB';
}

// ---------------- Floyd-Steinberg Dithering ----------------
function dither(c,threshold){
var img=c.getImageData(0,0,W,H);
var d=img.data;
var gray=new Float32Array(W*H);
for(var i=0;i<W*H;i++){
gray[i]=(d[i*4]*0.299+d[i*4+1]*0.587+d[i*4+2]*0.114);
}
var thr=threshold;
for(var y=0;y<H;y++){
for(var x=0;x<W;x++){
var idx=y*W+x;
var old=gray[idx];
var nv=old<thr?0:255;
gray[idx]=nv;
var err=old-nv;
if(x+1<W)   gray[idx+1]      += err*7/16;
if(y+1<H){
if(x>0)   gray[idx+W-1]    += err*3/16;
          gray[idx+W]      += err*5/16;
if(x+1<W) gray[idx+W+1]    += err*1/16;
}
}
}
var out=c.createImageData(W,H);
for(var i=0;i<W*H;i++){
var v=gray[i]>=thr?255:0;
out.data[i*4]=v;
out.data[i*4+1]=v;
out.data[i*4+2]=v;
out.data[i*4+3]=255;
}
c.putImageData(out,0,0);
}

// ---------------- Extract frames & upload ----------------
function packFrame(imgData){
// Returns 1024 bytes: 16 bytes × 64 rows, MSB-first per byte
var out=new Uint8Array(1024);
var d=imgData.data;
for(var y=0;y<H;y++){
for(var x=0;x<W;x++){
var v=(d[(y*W+x)*4]>=128)?1:0;
if(v){
var byteIdx=y*16+(x>>3);
var bit=7-(x&7);
out[byteIdx]|=(1<<bit);
}
}
}
return out;
}

$('btn-process').addEventListener('click',async function(){
if(!video.duration){toast('Select video first',1);return;}
var fps=parseInt($('fps').value);
var durSec=parseInt($('dur').value);
var frames=Math.floor(durSec*fps);
var fileName=prompt('Animation name (without .anim):','myanim');
if(!fileName)return;
fileName=fileName.trim().replace(/[^a-zA-Z0-9_-]/g,'');
if(!fileName){toast('Invalid name',1);return;}

$('prog').classList.add('show');
$('btn-process').disabled=true;

// Allocate buffer
var header=new Uint8Array(16);
header[0]=0x41;header[1]=0x4E;header[2]=0x49;header[3]=0x4D;  // "ANIM"
header[4]=W&0xFF;header[5]=(W>>8)&0xFF;
header[6]=H&0xFF;header[7]=(H>>8)&0xFF;
header[8]=fps&0xFF;header[9]=(fps>>8)&0xFF;
header[10]=frames&0xFF;
header[11]=(frames>>8)&0xFF;
header[12]=(frames>>16)&0xFF;
header[13]=(frames>>24)&0xFF;

var frameBufs=[];
var thr=parseInt($('thr').value);

// Pause video, seek through
video.pause();

for(var i=0;i<frames;i++){
var t=i/fps;
video.currentTime=t;
// Wait for seek
await new Promise(function(res){
var h=function(){video.removeEventListener('seeked',h);res();};
video.addEventListener('seeked',h);
});
// Draw to a temp canvas
var tmp=document.createElement('canvas');
tmp.width=W;tmp.height=H;
var tctx=tmp.getContext('2d');
tctx.drawImage(video,0,0,W,H);
dither(tctx,thr);
var imgData=tctx.getImageData(0,0,W,H);
frameBufs.push(packFrame(imgData));

// Update progress
if(i%5===0){
var pct=Math.floor((i/frames)*100);
$('prog-fill').style.width=pct+'%';
$('prog-lbl').textContent=pct+'%';
$('prog-status').textContent='Extracting frame '+i+'/'+frames;
}
}

// Concatenate into one blob
var totalSize=16+frames*1024;
var blob=new Uint8Array(totalSize);
blob.set(header,0);
var off=16;
for(var i=0;i<frames;i++){
blob.set(frameBufs[i],off);
off+=1024;
}

// Upload
$('prog-status').textContent='Uploading '+fileName+'.anim ('+(totalSize/1048576).toFixed(2)+' MB)...';
$('prog-fill').style.width='0%';
$('prog-lbl').textContent='0%';

var xhr=new XMLHttpRequest();
xhr.open('POST','/anim/upload');
xhr.upload.onprogress=function(e){
if(!e.lengthComputable)return;
var pct=Math.floor(e.loaded/e.total*100);
$('prog-fill').style.width=pct+'%';
$('prog-lbl').textContent='Upload: '+pct+'%';
};
xhr.onload=function(){
if(xhr.status===200){
$('prog-fill').style.width='100%';
$('prog-lbl').textContent='100%';
$('prog-status').textContent='Saved '+fileName+'.anim on ESP32';
toast('Upload successful!');
} else {
toast('Upload failed: '+xhr.status,1);
$('prog-status').textContent='Error '+xhr.status;
}
$('btn-process').disabled=false;
};
xhr.onerror=function(){
toast('Network error',1);
$('prog-status').textContent='Network error';
$('btn-process').disabled=false;
};

var fd=new FormData();
fd.append('file',new Blob([blob],{type:'application/octet-stream'}),fileName+'.anim');
xhr.send(fd);
});

// ---------------- Init ----------------
$('ip').textContent=location.host;
</script>
</body>
</html>
)rawliteral";
