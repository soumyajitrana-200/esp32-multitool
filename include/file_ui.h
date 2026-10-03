#pragma once
// SOUMYA Gadget — File Manager Web UI
// Served at http://192.168.4.1/files

const char FILE_MANAGER_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>SOUMYA Files</title>
<style>
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{background:#050505;color:#fff;font-family:-apple-system,'Segoe UI',sans-serif;padding:12px;min-height:100vh}
.header{display:flex;justify-content:space-between;align-items:center;padding:8px 0 16px;border-bottom:1px solid #00e5ff33;margin-bottom:14px}
.header h1{font-size:15px;color:#00e5ff;letter-spacing:2px;font-weight:700}
.header .ip{font-size:11px;color:#666;font-family:monospace}
.card{background:#0d0d0d;border:1px solid #1a1a1a;border-radius:14px;padding:14px;margin-bottom:12px}
.card h3{font-size:11px;color:#00e5ff;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:12px;font-weight:600}
.storage .row{display:flex;justify-content:space-between;font-size:12px;color:#888;margin-bottom:8px}
.storage .row .used{color:#00e5ff;font-weight:600;font-family:monospace}
.bar{width:100%;background:#1a1a1a;height:8px;border-radius:4px;overflow:hidden}
.bar .fill{background:linear-gradient(90deg,#00e5ff,#ff00aa);height:100%;transition:width .5s}
.upload{border:2px dashed #00e5ff44;border-radius:12px;padding:20px;text-align:center;position:relative}
.upload.dragging{border-color:#00e5ff;background:#00e5ff11}
.upload input[type=file]{position:absolute;inset:0;opacity:0;cursor:pointer}
.upload .icon{font-size:28px;color:#00e5ff;margin-bottom:8px}
.upload .label{font-size:13px;color:#00e5ff;font-weight:600}
.upload .hint{font-size:11px;color:#666;margin-top:4px}
.progress{margin-top:12px;display:none}
.progress.show{display:block}
.progress .name{font-size:11px;color:#aaa;margin-bottom:6px}
.progress .bar{height:6px}
.progress .bar .fill{background:#00e5ff}
.progress .pct{font-size:11px;color:#00e5ff;text-align:right;margin-top:4px}
.file{display:flex;align-items:center;gap:10px;padding:11px 0;border-bottom:1px solid #151515}
.file:last-child{border-bottom:none}
.file .ico{width:32px;height:32px;border-radius:8px;background:#1a1a1a;display:flex;align-items:center;justify-content:center;font-size:15px;color:#00e5ff}
.file .info{flex:1;min-width:0}
.file .name{font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.file .size{font-size:10px;color:#666;margin-top:2px}
.file .actions{display:flex;gap:6px}
.file .actions button{background:#1a1a1a;border:1px solid #222;color:#fff;width:32px;height:32px;border-radius:8px;font-size:12px}
.file .actions button.del{color:#ff3366;border-color:#ff336655}
.file .actions button:active{transform:scale(0.92);background:#00e5ff;color:#000}
.empty{text-align:center;color:#555;font-size:12px;padding:24px;font-style:italic}
.footer{text-align:center;font-size:10px;color:#444;padding:14px 0 6px}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(100px);background:#00e5ff;color:#000;padding:10px 20px;border-radius:20px;font-size:12px;font-weight:600;transition:.3s;z-index:999}
.toast.show{transform:translateX(-50%) translateY(0)}
.toast.err{background:#ff3366;color:#fff}
</style>
</head>
<body>

<div class="header">
<h1>SOUMYA FILES</h1>
<div class="ip">192.168.4.1</div>
</div>

<div class="card">
<h3>Storage</h3>
<div class="storage">
<div class="row">
<span>Used: <span class="used" id="used">&mdash;</span></span>
<span>Free: <span id="free">&mdash;</span></span>
</div>
<div class="bar"><div class="fill" id="storage-fill" style="width:0%"></div></div>
</div>
</div>

<div class="card">
<h3>Upload MP3</h3>
<div class="upload" id="upload-zone">
<div class="icon">&#11014;</div>
<div class="label">Tap to select MP3</div>
<div class="hint">or drag &amp; drop file here</div>
<input type="file" id="file-input" accept=".mp3,audio/mpeg">
</div>
<div class="progress" id="progress">
<div class="name" id="prog-name">&mdash;</div>
<div class="bar"><div class="fill" id="prog-fill" style="width:0%"></div></div>
<div class="pct" id="prog-pct">0%</div>
</div>
</div>

<div class="card">
<h3>Files (<span id="file-count">0</span>)</h3>
<div id="file-list"><div class="empty">Loading&hellip;</div></div>
</div>

<div class="footer">SOUMYA Gadget v9.1 &bull; File Manager</div>
<div class="toast" id="toast">OK</div>

<script>
var $=function(i){return document.getElementById(i)};

function esc(s){return s.replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}

function fmtSize(b){
if(b<1024)return b+' B';
if(b<1048576)return (b/1024).toFixed(1)+' KB';
return (b/1048576).toFixed(2)+' MB';
}

function toast(m,e){
var t=$('toast');
t.textContent=m;
t.className='toast'+(e?' err':'');
t.classList.add('show');
clearTimeout(t._t);
t._t=setTimeout(function(){t.classList.remove('show')},2000);
}

function loadList(){
fetch('/files/list').then(function(r){return r.json()}).then(function(j){
var usedMB=j.totalMB-j.freeMB;
var pct=j.totalMB?(usedMB/j.totalMB*100):0;
$('used').textContent=usedMB+' MB';
$('free').textContent=j.freeMB+' MB';
$('storage-fill').style.width=pct.toFixed(1)+'%';

$('file-count').textContent=j.files.length;
if(!j.files.length){
$('file-list').innerHTML='<div class="empty">No files yet</div>';
return;
}
$('file-list').innerHTML=j.files.map(function(f){
return '<div class="file">'+
'<div class="ico">&#9835;</div>'+
'<div class="info"><div class="name">'+esc(f.name)+'</div>'+
'<div class="size">'+fmtSize(f.size)+'</div></div>'+
'<div class="actions">'+
'<button onclick="downloadFile(\''+encodeURIComponent(f.name)+'\')">&#11015;</button>'+
'<button class="del" onclick="deleteFile(\''+encodeURIComponent(f.name)+'\')">&#10005;</button>'+
'</div></div>';
}).join('');
}).catch(function(){toast('Cannot reach device',1)});
}

function deleteFile(encName){
var name=decodeURIComponent(encName);
if(!confirm('Delete '+name+'?'))return;
fetch('/files/delete',{
method:'POST',
headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:'name='+encodeURIComponent(name)
}).then(function(r){
if(r.ok){toast('Deleted');loadList()}
else toast('Delete failed',1);
}).catch(function(){toast('Network error',1)});
}

function downloadFile(encName){
location.href='/files/download?name='+encName;
}

function uploadFile(file){
if(!file.name.toLowerCase().endsWith('.mp3')){
toast('Only MP3 allowed',1);
return;
}
var prog=$('progress');
prog.classList.add('show');
$('prog-name').textContent=file.name;
$('prog-fill').style.width='0%';
$('prog-pct').textContent='0%';

var xhr=new XMLHttpRequest();
xhr.open('POST','/files/upload');
xhr.upload.onprogress=function(e){
if(!e.lengthComputable)return;
var pct=Math.round(e.loaded/e.total*100);
$('prog-fill').style.width=pct+'%';
$('prog-pct').textContent=pct+'%';
};
xhr.onload=function(){
if(xhr.status===200){
toast('Uploaded '+file.name);
setTimeout(function(){prog.classList.remove('show');loadList()},500);
}else{
toast('Upload failed: '+xhr.status,1);
}
$('file-input').value='';
};
xhr.onerror=function(){toast('Upload error',1);$('file-input').value=''};

var fd=new FormData();
fd.append('file',file,file.name);
xhr.send(fd);
}

$('file-input').addEventListener('change',function(e){
if(e.target.files.length)uploadFile(e.target.files[0]);
});

var zone=$('upload-zone');
['dragenter','dragover'].forEach(function(ev){
zone.addEventListener(ev,function(e){e.preventDefault();zone.classList.add('dragging')});
});
['dragleave','drop'].forEach(function(ev){
zone.addEventListener(ev,function(e){e.preventDefault();zone.classList.remove('dragging')});
});
zone.addEventListener('drop',function(e){
if(e.dataTransfer.files.length)uploadFile(e.dataTransfer.files[0]);
});

loadList();
setInterval(loadList,5000);
</script>
</body>
</html>
)rawliteral";
