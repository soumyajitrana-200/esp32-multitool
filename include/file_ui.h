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
  body { background: #050505; color: #fff; font-family: 'Segoe UI', -apple-system, sans-serif; padding: 12px; min-height: 100vh; }
  .header { display: flex; justify-content: space-between; align-items: center; padding: 8px 0 16px; border-bottom: 1px solid #00e5ff33; margin-bottom: 16px; }
  .header h1 { font-size: 16px; color: #00e5ff; letter-spacing: 2px; text-shadow: 0 0 8px #00e5ff66; }
  .header .ip { font-size: 11px; color: #666; }
  .card { background: #0d0d0d; border: 1px solid #1a1a1a; border-radius: 14px; padding: 16px; margin-bottom: 14px; }
  .card h3 { font-size: 12px; color: #00e5ff; text-transform: uppercase; letter-spacing: 1.5px; margin-bottom: 12px; font-weight: 600; }
  .storage .row { display: flex; justify-content: space-between; font-size: 12px; color: #888; margin-bottom: 8px; }
  .storage .row .used { color: #00e5ff; font-weight: 600; }
  .bar { width: 100%; background: #1a1a1a; height: 8px; border-radius: 4px; overflow: hidden; }
  .bar .fill { background: linear-gradient(90deg, #00e5ff, #ff00aa); height: 100%; border-radius: 4px; transition: width 0.5s; }
  .upload { border: 2px dashed #00e5ff44; border-radius: 12px; padding: 20px; text-align: center; position: relative; transition: 0.3s; }
  .upload.dragging { border-color: #00e5ff; background: #00e5ff11; }
  .upload input[type=file] { position: absolute; top: 0; left: 0; width: 100%; height: 100%; opacity: 0; cursor: pointer; }
  .upload .icon { font-size: 32px; color: #00e5ff; margin-bottom: 8px; }
  .upload .label { font-size: 13px; color: #00e5ff; font-weight: 600; }
  .upload .hint { font-size: 11px; color: #666; margin-top: 4px; }
  .progress { margin-top: 12px; display: none; }
  .progress.show { display: block; }
  .progress .name { font-size: 12px; color: #aaa; margin-bottom: 6px; }
  .progress .pct { font-size: 11px; color: #00e5ff; text-align: right; margin-top: 4px; }
  .file { display: flex; align-items: center; gap: 10px; padding: 12px 0; border-bottom: 1px solid #151515; }
  .file:last-child { border-bottom: none; }
  .file .ico { width: 32px; height: 32px; border-radius: 8px; background: #1a1a1a; display: flex; align-items: center; justify-content: center; font-size: 16px; flex-shrink: 0; }
  .file .info { flex: 1; min-width: 0; }
  .file .name { font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .file .size { font-size: 11px; color: #666; margin-top: 2px; }
  .file .actions { display: flex; gap: 6px; }
  .file .actions button { background: #1a1a1a; border: 1px solid #222; color: #fff; width: 34px; height: 34px; border-radius: 8px; font-size: 13px; transition: 0.2s; }
  .file .actions button.del { color: #ff3366; border-color: #ff336655; }
  .empty { text-align: center; color: #555; font-size: 12px; padding: 24px; }
  .footer { text-align: center; font-size: 10px; color: #444; padding: 16px 0 8px; }
  .toast { position: fixed; bottom: 24px; left: 50%; transform: translateX(-50%) translateY(100px); background: #00e5ff; color: #000; padding: 10px 20px; border-radius: 20px; font-size: 12px; font-weight: 600; transition: 0.3s; z-index: 999; }
  .toast.show { transform: translateX(-50%) translateY(0); }
  .toast.error { background: #ff3366; color: #fff; }
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
  <h3>Upload</h3>
  <div class="upload" id="upload-zone">
    <div class="icon">⬆</div>
    <div class="label">Tap to select MP3</div>
    <div class="hint">or drag & drop file here</div>
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

<div class="footer">SOUMYA Gadget v9.0 • File Manager</div>
<div class="toast" id="toast">OK</div>

<script>
function toast(msg, isError) {
  const t = document.getElementById('toast');
  t.innerText = msg;
  t.classList.toggle('error', !!isError);
  t.classList.add('show');
  clearTimeout(window._toastT);
  window._toastT = setTimeout(() => t.classList.remove('show'), 2000);
}
function fmtSize(bytes) {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}
function loadFiles() {
  fetch('/list').then(r => r.json()).then(data => {
    document.getElementById('used').innerText = (data.totalMB - data.freeMB) + ' MB';
    document.getElementById('free').innerText = data.freeMB + ' MB';
    document.getElementById('file-count').innerText = data.files.length;
    const usedPct = data.totalMB > 0 ? ((data.totalMB - data.freeMB) / data.totalMB * 100) : 0;
    document.getElementById('storage-fill').style.width = usedPct + '%';
    const list = document.getElementById('file-list');
    if (data.files.length === 0) {
      list.innerHTML = '<div class="empty">No files. Upload an MP3!</div>';
      return;
    }
    let html = '';
    data.files.forEach(f => {
      html += '<div class="file">';
      html += '<div class="ico">♪</div>';
      html += '<div class="info"><div class="name">' + f.name + '</div><div class="size">' + fmtSize(f.size) + '</div></div>';
      html += '<div class="actions">';
      html += '<button onclick="downloadFile(\'' + f.name.replace(/'/g, "\\'") + '\')">⬇</button>';
      html += '<button class="del" onclick="deleteFile(\'' + f.name.replace(/'/g, "\\'") + '\')">✕</button>';
      html += '</div></div>';
    });
    list.innerHTML = html;
  }).catch(e => {
    document.getElementById('file-list').innerHTML = '<div class="empty">Error loading files. SD card inserted?</div>';
  });
}
function onFileSelected(input) {
  if (!input.files || input.files.length === 0) return;
  const file = input.files[0];
  if (!file.name.toLowerCase().endsWith('.mp3')) { toast('Only MP3 allowed', true); return; }
  uploadFile(file);
}
function uploadFile(file) {
  const form = new FormData();
  form.append('file', file, file.name);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/upload', true);
  const prog = document.getElementById('progress');
  prog.classList.add('show');
  document.getElementById('prog-name').innerText = file.name;
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      document.getElementById('prog-fill').style.width = pct + '%';
      document.getElementById('prog-pct').innerText = pct + '%';
    }
  };
  xhr.onload = () => {
    if (xhr.status === 200) {
      toast('Uploaded ' + file.name);
      setTimeout(() => { prog.classList.remove('show'); document.getElementById('file-input').value = ''; loadFiles(); }, 800);
    } else { toast('Upload failed', true); prog.classList.remove('show'); }
  };
  xhr.onerror = () => { toast('Network error', true); prog.classList.remove('show'); };
  xhr.send(form);
}
function deleteFile(name) {
  if (!confirm('Delete ' + name + '?')) return;
  fetch('/delete', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'name=' + encodeURIComponent(name)
  }).then(() => { toast('Deleted'); loadFiles(); })
    .catch(() => toast('Delete failed', true));
}
function downloadFile(name) {
  window.location.href = '/download?name=' + encodeURIComponent(name);
  toast('Downloading...');
}
const zone = document.getElementById('upload-zone');
['dragenter', 'dragover'].forEach(ev => {
  zone.addEventListener(ev, e => { e.preventDefault(); zone.classList.add('dragging'); });
});
['dragleave', 'drop'].forEach(ev => {
  zone.addEventListener(ev, e => { e.preventDefault(); zone.classList.remove('dragging'); });
});
zone.addEventListener('drop', e => {
  if (e.dataTransfer.files.length > 0) {
    const file = e.dataTransfer.files[0];
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
