#ifndef FILE_UI_H
#define FILE_UI_H

const char FILE_MANAGER_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>SOUMYA Files</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body { background: #050505; color: #fff; font-family: -apple-system, 'Segoe UI', sans-serif; padding: 12px; min-height: 100vh; }
  .header { display: flex; justify-content: space-between; align-items: center; padding: 8px 0 16px; border-bottom: 1px solid #00e5ff33; margin-bottom: 14px; }
  .header h1 { font-size: 15px; color: #00e5ff; letter-spacing: 2px; text-shadow: 0 0 10px #00e5ff66; font-weight: 700; }
  .header .ip { font-size: 11px; color: #666; font-family: monospace; }
  .card { background: #0d0d0d; border: 1px solid #1a1a1a; border-radius: 14px; padding: 14px; margin-bottom: 12px; }
  .card h3 { font-size: 11px; color: #00e5ff; text-transform: uppercase; letter-spacing: 1.5px; margin-bottom: 12px; font-weight: 600; }
  .storage .row { display: flex; justify-content: space-between; font-size: 12px; color: #888; margin-bottom: 8px; }
  .storage .row .used { color: #00e5ff; font-weight: 600; font-family: monospace; }
  .bar { width: 100%; background: #1a1a1a; height: 8px; border-radius: 4px; overflow: hidden; }
  .bar .fill { background: linear-gradient(90deg, #00e5ff, #ff00aa); height: 100%; border-radius: 4px; transition: width 0.5s; }
  .upload { border: 2px dashed #00e5ff44; border-radius: 12px; padding: 20px; text-align: center; position: relative; transition: 0.3s; }
  .upload.dragging { border-color: #00e5ff; background: #00e5ff11; }
  .upload input[type=file] { position: absolute; top: 0; left: 0; width: 100%; height: 100%; opacity: 0; cursor: pointer; }
  .upload .icon { font-size: 28px; color: #00e5ff; margin-bottom: 8px; }
  .upload .label { font-size: 13px; color: #00e5ff; font-weight: 600; }
  .upload .hint { font-size: 11px; color: #666; margin-top: 4px; }
  .progress { margin-top: 12px; display: none; }
  .progress.show { display: block; }
  .progress .name { font-size: 11px; color: #aaa; margin-bottom: 6px; }
  .progress .bar { height: 6px; }
  .progress .bar .fill { background: #00e5ff; }
  .progress .pct { font-size: 11px; color: #00e5ff; text-align: right; margin-top: 4px; font-family: monospace; }
  .file { display: flex; align-items: center; gap: 10px; padding: 11px 0; border-bottom: 1px solid #151515; }
  .file:last-child { border-bottom: none; }
  .file .ico { width: 32px; height: 32px; border-radius: 8px; background: #1a1a1a; display: flex; align-items: center; justify-content: center; font-size: 15px; flex-shrink: 0; color: #00e5ff; }
  .file .info { flex: 1; min-width: 0; }
  .file .name { font-size: 12px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .file .size { font-size: 10px; color: #666; margin-top: 2px; font-family: monospace; }
  .file .actions { display: flex; gap: 6px; }
  .file .actions button { background: #1a1a1a; border: 1px solid #222; color: #fff; width: 32px; height: 32px; border-radius: 8px; font-size: 12px; transition: 0.2s; }
  .file .actions button.del { color: #ff3366; border-color: #ff336655; }
  .file .actions button:active { transform: scale(0.92); background: #00e5ff; color: #000; }
  .file .actions button.del:active { background: #ff3366; color: #fff; }
  .empty { text-align: center; color: #555; font-size: 12px; padding: 24px; font-style: italic; }
  .footer { text-align: center; font-size: 10px; color: #444; padding: 14px 0 6px; }
  .toast { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%) translateY(100px); background: #00e5ff; color: #000; padding: 10px 20px; border-radius: 20px; font-size: 12px; font-weight: 600; transition: 0.3s; z-index: 999; }
  .toast.show { transform: translateX(-50%) translateY(0); }
  .toast.err { background: #ff3366; color: #fff; }
</style>
</head>
<body>

<div class="header">
  <h1>SOUMYA FILES</h1>
  <div class="ip">:81</div>
</div>

<div class="card">
  <h3>Storage</h3>
  <div class="storage">
    <div class="row">
      <span>Used: <span class="used" id="used">-- MB</span></span>
      <span>Free: <span id="free">-- MB</span></span>
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
    <input type="file" id="file-input" accept=".mp3,audio/mpeg" onchange="onFileSelected(this)">
  </div>
  <div class="progress" id="progress">
    <div class="name" id="prog-name">filename.mp3</div>
    <div class="bar"><div class="fill" id="prog-fill" style="width:0%"></div></div>
    <div class="pct" id="prog-pct">0%</div>
  </div>
</div>

<div class="card">
  <h3>Files (<span id="file-count">0</span>)</h3>
  <div id="file-list"><div class="empty">Loading...</div></div>
</div>

<div class="footer">SOUMYA Gadget v9.0 &bull; File Manager</div>
<div class="toast" id="toast">OK</div>

<script>
function toast(msg, isErr) {
  var t = document.getElementById('toast');
  t.innerText = msg;
  if (isErr) t.classList.add('err'); else t.classList.remove('err');
  t.classList.add('show');
  clearTimeout(window._tt);
  window._tt = setTimeout(function(){ t.classList.remove('show'); }, 2000);
}

function fmtSize(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

function escJs(s) {
  return s.replace(/\\/g, '\\\\').replace(/'/g, "\\'");
}

function loadFiles() {
  fetch('/list').then(function(r){ return r.json(); }).then(function(data) {
    document.getElementById('used').innerText = (data.totalMB - data.freeMB) + ' MB';
    document.getElementById('free').innerText = data.freeMB + ' MB';
    document.getElementById('file-count').innerText = data.files.length;
    var usedPct = data.totalMB > 0 ? ((data.totalMB - data.freeMB) / data.totalMB * 100) : 0;
    document.getElementById('storage-fill').style.width = usedPct + '%';

    var list = document.getElementById('file-list');
    if (data.files.length === 0) {
      list.innerHTML = '<div class="empty">No files. Upload an MP3!</div>';
      return;
    }
    var html = '';
    for (var i = 0; i < data.files.length; i++) {
      var f = data.files[i];
      html += '<div class="file">';
      html += '<div class="ico">&#9835;</div>';
      html += '<div class="info"><div class="name">' + f.name + '</div><div class="size">' + fmtSize(f.size) + '</div></div>';
      html += '<div class="actions">';
      html += '<button onclick="downloadFile(\'' + escJs(f.name) + '\')">&#11015;</button>';
      html += '<button class="del" onclick="deleteFile(\'' + escJs(f.name) + '\')">&#10005;</button>';
      html += '</div></div>';
    }
    list.innerHTML = html;
  }).catch(function(e) {
    document.getElementById('file-list').innerHTML = '<div class="empty">Error loading files. SD card inserted?</div>';
  });
}

function onFileSelected(input) {
  if (!input.files || input.files.length === 0) return;
  var file = input.files[0];
  if (!file.name.toLowerCase().endsWith('.mp3')) {
    toast('Only MP3 allowed', true);
    return;
  }
  uploadFile(file);
}

function uploadFile(file) {
  var form = new FormData();
  form.append('file', file, file.name);
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/upload', true);

  var prog = document.getElementById('progress');
  prog.classList.add('show');
  document.getElementById('prog-name').innerText = file.name;
  document.getElementById('prog-fill').style.width = '0%';
  document.getElementById('prog-pct').innerText = '0%';

  xhr.upload.onprogress = function(e) {
    if (e.lengthComputable) {
      var pct = Math.round((e.loaded / e.total) * 100);
      document.getElementById('prog-fill').style.width = pct + '%';
      document.getElementById('prog-pct').innerText = pct + '%';
    }
  };
  xhr.onload = function() {
    if (xhr.status === 200) {
      toast('Uploaded ' + file.name);
      document.getElementById('prog-fill').style.width = '100%';
      document.getElementById('prog-pct').innerText = '100%';
      setTimeout(function() {
        prog.classList.remove('show');
        document.getElementById('file-input').value = '';
        loadFiles();
      }, 800);
    } else {
      toast('Upload failed', true);
      prog.classList.remove('show');
    }
  };
  xhr.onerror = function() {
    toast('Network error', true);
    prog.classList.remove('show');
  };
  xhr.send(form);
}

function deleteFile(name) {
  if (!confirm('Delete ' + name + '?')) return;
  fetch('/delete', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'name=' + encodeURIComponent(name)
  }).then(function() {
    toast('Deleted');
    loadFiles();
  }).catch(function() {
    toast('Delete failed', true);
  });
}

function downloadFile(name) {
  window.location.href = '/download?name=' + encodeURIComponent(name);
  toast('Downloading...');
}

var zone = document.getElementById('upload-zone');
var dragEvents = ['dragenter', 'dragover'];
for (var i = 0; i < dragEvents.length; i++) {
  zone.addEventListener(dragEvents[i], function(e){ e.preventDefault(); zone.classList.add('dragging'); });
}
var dropEvents = ['dragleave', 'drop'];
for (var j = 0; j < dropEvents.length; j++) {
  zone.addEventListener(dropEvents[j], function(e){ e.preventDefault(); zone.classList.remove('dragging'); });
}
zone.addEventListener('drop', function(e) {
  if (e.dataTransfer.files.length > 0) {
    var file = e.dataTransfer.files[0];
    if (file.name.toLowerCase().endsWith('.mp3')) uploadFile(file);
    else toast('Only MP3 allowed', true);
  }
});

loadFiles();
</script>
</body>
</html>
)rawliteral";

#endif
