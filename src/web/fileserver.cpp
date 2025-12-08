// WiFi File Server implementation

#include "fileserver.h"
#include <SD.h>
#include <ESPmDNS.h>

// Static members
WebServer* FileServer::server = nullptr;
FileServerState FileServer::state = FileServerState::IDLE;
char FileServer::statusMessage[64] = "Ready";
char FileServer::targetSSID[64] = "";
char FileServer::targetPassword[64] = "";
uint32_t FileServer::connectStartTime = 0;
uint32_t FileServer::lastReconnectCheck = 0;

// File upload state (needs to be declared early for stop() to access it)
static File uploadFile;
static String uploadDir;

// Black & white HTML interface with full filesystem navigation
static const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PORKCHOP File Manager</title>
    <style>
        :root { --pink: #FFAEAD; --bg: #000; }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { 
            background: var(--bg); 
            color: var(--pink); 
            font-family: 'Courier New', monospace;
            font-size: 1.0em;
            padding: 20px;
            max-width: 900px;
            margin: 0 auto;
        }
        .logo {
            white-space: pre;
            font-size: 0.35em;
            line-height: 1.1;
            margin-bottom: 10px;
            overflow-x: auto;
        }
        .header {
            border-bottom: 2px solid var(--pink);
            padding-bottom: 10px;
            margin-bottom: 15px;
        }
        .sd-info {
            opacity: 0.6;
            font-size: 0.85em;
            margin-top: 5px;
        }
        .breadcrumb {
            margin: 10px 0;
            padding: 10px 12px;
            background: #0a0505;
            border: 1px solid #331a1a;
            font-size: 0.95em;
        }
        .breadcrumb a { color: var(--pink); text-decoration: none; }
        .breadcrumb a:hover { text-decoration: underline; }
        .breadcrumb span { opacity: 0.5; }
        .file-list {
            border: 1px solid #331a1a;
            margin: 10px 0;
        }
        .file-item { 
            display: flex; 
            justify-content: space-between; 
            align-items: center;
            padding: 12px 10px;
            border-bottom: 1px solid #1a0d0d;
            transition: background 0.1s;
        }
        .file-item:hover { background: #0f0808; }
        .file-item:last-child { border-bottom: none; }
        .file-icon { 
            width: 24px; 
            text-align: center; 
            margin-right: 8px; 
            opacity: 0.6;
        }
        .file-icon.dir { opacity: 1; }
        .file-name { flex: 1; overflow: hidden; text-overflow: ellipsis; }
        .file-name a { color: var(--pink); text-decoration: none; }
        .file-name a:hover { text-decoration: underline; }
        .file-size { opacity: 0.5; margin: 0 15px; min-width: 70px; text-align: right; font-size: 0.85em; }
        .file-actions { display: flex; gap: 5px; }
        .btn {
            background: var(--pink);
            color: var(--bg);
            border: none;
            padding: 6px 14px;
            cursor: pointer;
            font-family: inherit;
            font-size: 0.85em;
            transition: opacity 0.15s;
        }
        .btn:hover { opacity: 0.8; }
        .btn:active { opacity: 0.6; }
        .btn-outline { background: transparent; color: var(--pink); border: 1px solid var(--pink); opacity: 0.7; }
        .btn-outline:hover { opacity: 1; background: #1a0d0d; }
        .btn-danger { background: transparent; color: var(--pink); border: 1px solid var(--pink); opacity: 0.5; }
        .btn-danger:hover { opacity: 1; background: #1a0505; }
        .btn-sm { padding: 4px 10px; font-size: 0.8em; }
        .toolbar {
            display: flex;
            gap: 8px;
            margin: 12px 0;
            flex-wrap: wrap;
            align-items: center;
        }
        .upload-btn {
            position: relative;
            overflow: hidden;
        }
        .upload-btn input[type="file"] {
            position: absolute;
            left: 0; top: 0;
            width: 100%; height: 100%;
            opacity: 0;
            cursor: pointer;
        }
        .dropzone {
            border: 2px dashed #331a1a;
            padding: 20px;
            text-align: center;
            opacity: 0.5;
            margin: 10px 0;
            transition: all 0.2s;
            display: none;
        }
        .dropzone.active {
            display: block;
        }
        .dropzone.dragover {
            border-color: var(--pink);
            opacity: 1;
            background: #0a0505;
        }
        .progress-container {
            margin: 10px 0;
            display: none;
        }
        .progress-container.active { display: block; }
        .progress-bar {
            width: 100%;
            height: 6px;
            background: #1a0d0d;
            border-radius: 3px;
            overflow: hidden;
        }
        .progress-fill {
            height: 100%;
            background: var(--pink);
            width: 0%;
            transition: width 0.2s;
        }
        .progress-text {
            font-size: 0.8em;
            opacity: 0.6;
            margin-top: 5px;
        }
        .status { 
            font-size: 0.85em;
            padding: 8px 0;
            min-height: 24px;
            opacity: 0.7;
        }
        .status.success { opacity: 1; }
        .status.error { opacity: 1; }
        input[type="text"] {
            background: #0a0505;
            color: var(--pink);
            border: 1px solid #331a1a;
            padding: 8px 12px;
            font-family: inherit;
            font-size: 1em;
            width: 100%;
        }
        input[type="text"]:focus {
            outline: none;
            border-color: var(--pink);
        }
        .modal {
            display: none;
            position: fixed;
            top: 0; left: 0;
            width: 100%; height: 100%;
            background: rgba(0,0,0,0.9);
            justify-content: center;
            align-items: center;
            z-index: 100;
        }
        .modal-content {
            background: var(--bg);
            border: 1px solid var(--pink);
            padding: 25px;
            max-width: 350px;
            width: 90%;
        }
        .modal-content h3 { margin-bottom: 20px; font-weight: normal; }
        .modal-actions { display: flex; gap: 10px; margin-top: 20px; }
        .empty-state {
            padding: 40px;
            text-align: center;
            opacity: 0.4;
        }
        @media (max-width: 600px) {
            .logo { font-size: 0.25em; }
            .file-size { display: none; }
            .btn { padding: 8px 12px; }
        }
    </style>
</head>
<body>
    <div class="header">
        <pre class="logo"> ██▓███   ▒█████   ██▀███   ██ ▄█▀ ▄████▄   ██░ ██  ▒█████   ██▓███  
▓██░  ██▒▒██▒  ██▒▓██ ▒ ██▒ ██▄█▒ ▒██▀ ▀█  ▓██░ ██▒▒██▒  ██▒▓██░  ██▒
▓██░ ██▓▒▒██░  ██▒▓██ ░▄█ ▒▓███▄░ ▒▓█    ▄ ▒██▀▀██░▒██░  ██▒▓██░ ██▓▒
▒██▄█▓▒ ▒▒██   ██░▒██▀▀█▄  ▓██ █▄ ▒▓▓▄ ▄██▒░▓█ ░██ ▒██   ██░▒██▄█▓▒ ▒
▒██▒ ░  ░░ ████▓▒░░██▓ ▒██▒▒██▒ █▄▒ ▓███▀ ░░▓█▒░██▓░ ████▓▒░▒██▒ ░  ░
▒▓▒░ ░  ░░ ▒░▒░▒░ ░ ▒▓ ░▒▓░▒ ▒▒ ▓▒░ ░▒ ▒  ░ ▒ ░░▒░▒░ ▒░▒░▒░ ▒▓▒░ ░  ░
░▒ ░       ░ ▒ ▒░   ░▒ ░ ▒░░ ░▒ ▒░  ░  ▒    ▒ ░▒░ ░  ░ ▒ ▒░ ░▒ ░     
░░       ░ ░ ░ ▒    ░░   ░ ░ ░░ ░ ░         ░  ░░ ░░ ░ ░ ▒  ░░       
             ░ ░     ░     ░  ░   ░ ░       ░  ░  ░    ░ ░           
                                  ░                                  </pre>
        <div class="sd-info" id="sdInfo">Loading...</div>
    </div>
    
    <div class="breadcrumb" id="breadcrumb"></div>
    
    <div class="toolbar">
        <button class="btn btn-outline" onclick="loadDir(currentPath)">Refresh</button>
        <button class="btn btn-outline" onclick="showNewFolderModal()">+ Folder</button>
        <label class="btn upload-btn">
            Upload
            <input type="file" id="fileInput" name="file" multiple onchange="uploadFiles(this.files)">
        </label>
    </div>
    
    <div class="dropzone" id="dropzone">
        Drop files here to upload
    </div>
    
    <div class="progress-container" id="progressContainer">
        <div class="progress-bar">
            <div class="progress-fill" id="progressFill"></div>
        </div>
        <div class="progress-text" id="progressText">Uploading...</div>
    </div>
    
    <div class="file-list" id="fileList"></div>
    
    <div class="status" id="status"></div>
    
    <!-- New Folder Modal -->
    <div class="modal" id="newFolderModal" onclick="if(event.target===this)hideModal()">
        <div class="modal-content">
            <h3>New Folder</h3>
            <input type="text" id="newFolderName" placeholder="Folder name" onkeydown="if(event.key==='Enter')createFolder();if(event.key==='Escape')hideModal()">
            <div class="modal-actions">
                <button class="btn" onclick="createFolder()">Create</button>
                <button class="btn btn-outline" onclick="hideModal()">Cancel</button>
            </div>
        </div>
    </div>
    
    <script>
        let currentPath = '/';
        
        async function loadSDInfo() {
            try {
                const resp = await fetch('/api/sdinfo');
                const info = await resp.json();
                const pct = ((info.used / info.total) * 100).toFixed(0);
                document.getElementById('sdInfo').textContent = 
                    formatSize(info.used) + ' / ' + formatSize(info.total) + ' (' + pct + '% used)';
            } catch(e) {
                document.getElementById('sdInfo').textContent = 'SD card unavailable';
            }
        }
        
        function updateBreadcrumb() {
            const parts = currentPath.split('/').filter(p => p);
            let html = '<a href="#" onclick="loadDir(\'/\');return false;">~</a>';
            let path = '';
            for (const p of parts) {
                path += '/' + p;
                const safePath = path;
                html += ' <span>/</span> <a href="#" onclick="loadDir(\'' + safePath + '\');return false;">' + p + '</a>';
            }
            document.getElementById('breadcrumb').innerHTML = html;
        }
        
        async function loadDir(path) {
            currentPath = path || '/';
            updateBreadcrumb();
            
            const container = document.getElementById('fileList');
            container.innerHTML = '<div class="file-item" style="color:#666">Loading...</div>';
            
            try {
                const resp = await fetch('/api/ls?dir=' + encodeURIComponent(currentPath) + '&full=1');
                const items = await resp.json();
                
                let html = '';
                
                // Parent directory link
                if (currentPath !== '/') {
                    const parent = currentPath.substring(0, currentPath.lastIndexOf('/')) || '/';
                    html += '<div class="file-item">';
                    html += '<span class="file-icon dir">..</span>';
                    html += '<span class="file-name"><a href="#" onclick="loadDir(\'' + parent + '\');return false;">Parent Directory</a></span>';
                    html += '<span class="file-size"></span>';
                    html += '<div class="file-actions"></div>';
                    html += '</div>';
                }
                
                // Folders first
                for (const item of items.filter(i => i.isDir).sort((a,b) => a.name.localeCompare(b.name))) {
                    const itemPath = (currentPath === '/' ? '' : currentPath) + '/' + item.name;
                    const escapedPath = itemPath.replace(/'/g, "\\'");
                    html += '<div class="file-item">';
                    html += '<span class="file-icon dir">/</span>';
                    html += '<span class="file-name"><a href="#" onclick="loadDir(\'' + escapedPath + '\');return false;">' + escapeHtml(item.name) + '</a></span>';
                    html += '<span class="file-size"></span>';
                    html += '<div class="file-actions">';
                    html += '<button class="btn btn-danger btn-sm" onclick="del(\'' + escapedPath + '\', true)">Del</button>';
                    html += '</div></div>';
                }
                
                // Then files
                for (const item of items.filter(i => !i.isDir).sort((a,b) => a.name.localeCompare(b.name))) {
                    const itemPath = (currentPath === '/' ? '' : currentPath) + '/' + item.name;
                    const escapedPath = itemPath.replace(/'/g, "\\'");
                    html += '<div class="file-item">';
                    html += '<span class="file-icon">*</span>';
                    html += '<span class="file-name">' + escapeHtml(item.name) + '</span>';
                    html += '<span class="file-size">' + formatSize(item.size) + '</span>';
                    html += '<div class="file-actions">';
                    html += '<button class="btn btn-outline btn-sm" onclick="download(\'' + escapedPath + '\')">Get</button>';
                    html += '<button class="btn btn-danger btn-sm" onclick="del(\'' + escapedPath + '\', false)">Del</button>';
                    html += '</div></div>';
                }
                
                if (!html && currentPath === '/') {
                    html = '<div class="empty-state">No files yet</div>';
                } else if (!html) {
                    html = '<div class="empty-state">Empty folder</div>';
                }
                
                container.innerHTML = html;
            } catch (e) {
                container.innerHTML = '<div class="empty-state">Error loading directory</div>';
            }
        }
        
        function escapeHtml(str) {
            return str.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
        }
        
        function formatSize(bytes) {
            if (bytes < 1024) return bytes + ' B';
            if (bytes < 1024*1024) return (bytes/1024).toFixed(1) + ' KB';
            if (bytes < 1024*1024*1024) return (bytes/1024/1024).toFixed(1) + ' MB';
            return (bytes/1024/1024/1024).toFixed(2) + ' GB';
        }
        
        function download(path) {
            window.location.href = '/download?f=' + encodeURIComponent(path);
        }
        
        async function del(path, isDir) {
            const name = path.split('/').pop();
            const msg = isDir ? 'Delete folder "' + name + '" and all contents?' : 'Delete "' + name + '"?';
            if (!confirm(msg)) return;
            
            setStatus('Deleting...', '');
            const endpoint = isDir ? '/rmdir' : '/delete';
            const resp = await fetch(endpoint + '?f=' + encodeURIComponent(path));
            if (resp.ok) {
                setStatus('Deleted: ' + name, 'success');
                loadDir(currentPath);
            } else {
                setStatus('Delete failed', 'error');
            }
        }
        
        function setStatus(msg, type) {
            const el = document.getElementById('status');
            el.textContent = msg;
            el.className = 'status' + (type ? ' ' + type : '');
        }
        
        function showNewFolderModal() {
            document.getElementById('newFolderModal').style.display = 'flex';
            document.getElementById('newFolderName').value = '';
            document.getElementById('newFolderName').focus();
        }
        
        function hideModal() {
            document.getElementById('newFolderModal').style.display = 'none';
        }
        
        async function createFolder() {
            const name = document.getElementById('newFolderName').value.trim();
            if (!name) { alert('Enter folder name'); return; }
            if (name.includes('/') || name.includes('..')) { alert('Invalid name'); return; }
            
            const path = (currentPath === '/' ? '' : currentPath) + '/' + name;
            const resp = await fetch('/mkdir?f=' + encodeURIComponent(path));
            if (resp.ok) {
                setStatus('Created: ' + name, 'success');
                hideModal();
                loadDir(currentPath);
            } else {
                setStatus('Create folder failed', 'error');
            }
        }
        
        async function uploadFiles(files) {
            if (!files || !files.length) return;
            
            const container = document.getElementById('progressContainer');
            const fill = document.getElementById('progressFill');
            const text = document.getElementById('progressText');
            container.classList.add('active');
            
            let uploaded = 0;
            for (let i = 0; i < files.length; i++) {
                const file = files[i];
                text.textContent = 'Uploading ' + (i+1) + '/' + files.length + ': ' + file.name;
                fill.style.width = '0%';
                
                const formData = new FormData();
                formData.append('file', file);
                
                try {
                    await new Promise((resolve, reject) => {
                        const xhr = new XMLHttpRequest();
                        xhr.upload.onprogress = (e) => {
                            if (e.lengthComputable) {
                                fill.style.width = (e.loaded / e.total * 100) + '%';
                            }
                        };
                        xhr.onload = () => xhr.status === 200 ? resolve() : reject(new Error('Failed'));
                        xhr.onerror = () => reject(new Error('Network error'));
                        xhr.open('POST', '/upload?dir=' + encodeURIComponent(currentPath));
                        xhr.send(formData);
                    });
                    uploaded++;
                } catch (e) {
                    setStatus('Upload error: ' + file.name, 'error');
                }
            }
            
            container.classList.remove('active');
            document.getElementById('fileInput').value = '';
            
            if (uploaded === files.length) {
                setStatus('Uploaded ' + uploaded + ' file(s)', 'success');
            } else {
                setStatus('Uploaded ' + uploaded + '/' + files.length + ' files', uploaded > 0 ? 'success' : 'error');
            }
            loadDir(currentPath);
        }
        
        // Drag and drop
        const dropzone = document.getElementById('dropzone');
        let dragCounter = 0;
        
        document.body.addEventListener('dragenter', (e) => {
            e.preventDefault();
            dragCounter++;
            dropzone.classList.add('active');
        });
        
        document.body.addEventListener('dragleave', (e) => {
            dragCounter--;
            if (dragCounter === 0) dropzone.classList.remove('active');
        });
        
        document.body.addEventListener('dragover', (e) => {
            e.preventDefault();
            dropzone.classList.add('dragover');
        });
        
        dropzone.addEventListener('dragleave', () => {
            dropzone.classList.remove('dragover');
        });
        
        dropzone.addEventListener('drop', (e) => {
            e.preventDefault();
            dragCounter = 0;
            dropzone.classList.remove('active', 'dragover');
            if (e.dataTransfer.files.length) {
                uploadFiles(e.dataTransfer.files);
            }
        });
        
        // Initial load
        loadSDInfo();
        loadDir('/');
    </script>
</body>
</html>
)rawliteral";

void FileServer::init() {
    state = FileServerState::IDLE;
    strcpy(statusMessage, "Ready");
    targetSSID[0] = '\0';
    targetPassword[0] = '\0';
}

bool FileServer::start(const char* ssid, const char* password) {
    if (state != FileServerState::IDLE) return true;
    
    // Store credentials for reconnection
    strncpy(targetSSID, ssid ? ssid : "", sizeof(targetSSID) - 1);
    strncpy(targetPassword, password ? password : "", sizeof(targetPassword) - 1);
    
    // Check credentials
    if (strlen(targetSSID) == 0) {
        strcpy(statusMessage, "No WiFi SSID set");
        return false;
    }
    
    strcpy(statusMessage, "jacking in.");
    Serial.printf("[FILESERVER] Starting connection to %s\n", targetSSID);
    
    // Start non-blocking connection
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(targetSSID, targetPassword);
    
    state = FileServerState::CONNECTING;
    connectStartTime = millis();
    
    return true;
}

void FileServer::startServer() {
    snprintf(statusMessage, sizeof(statusMessage), "%s", WiFi.localIP().toString().c_str());
    Serial.printf("[FILESERVER] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    
    // Start mDNS
    if (MDNS.begin("porkchop")) {
        Serial.println("[FILESERVER] mDNS: porkchop.local");
    }
    
    // Create and configure web server
    server = new WebServer(80);
    
    server->on("/", HTTP_GET, handleRoot);
    server->on("/api/ls", HTTP_GET, handleFileList);
    server->on("/api/sdinfo", HTTP_GET, handleSDInfo);
    server->on("/download", HTTP_GET, handleDownload);
    server->on("/upload", HTTP_POST, handleUpload, handleUploadProcess);
    server->on("/delete", HTTP_GET, handleDelete);
    server->on("/rmdir", HTTP_GET, handleDelete);  // Same handler, will detect folder
    server->on("/mkdir", HTTP_GET, handleMkdir);
    server->on("/downloadzip", HTTP_GET, handleDownload);  // ZIP handled in download
    server->onNotFound(handleNotFound);
    
    server->begin();
    state = FileServerState::RUNNING;
    lastReconnectCheck = millis();
    
    Serial.println("[FILESERVER] Server started on port 80");
}

void FileServer::stop() {
    if (state == FileServerState::IDLE) return;
    
    // Close any pending upload file
    if (uploadFile) {
        uploadFile.close();
        Serial.println("[FILESERVER] Closed pending upload file");
    }
    
    if (server) {
        server->stop();
        delete server;
        server = nullptr;
    }
    
    MDNS.end();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    
    state = FileServerState::IDLE;
    strcpy(statusMessage, "Stopped");
    Serial.println("[FILESERVER] Stopped");
}

void FileServer::update() {
    switch (state) {
        case FileServerState::CONNECTING:
        case FileServerState::RECONNECTING:
            updateConnecting();
            break;
        case FileServerState::RUNNING:
            updateRunning();
            break;
        default:
            break;
    }
}

void FileServer::updateConnecting() {
    uint32_t elapsed = millis() - connectStartTime;
    
    if (WiFi.status() == WL_CONNECTED) {
        startServer();
        return;
    }
    
    // Update status with dots animation
    int dots = (elapsed / 500) % 4;
    snprintf(statusMessage, sizeof(statusMessage), "jacking in%.*s", dots, "...");
    
    // Timeout after 15 seconds
    if (elapsed > 15000) {
        strcpy(statusMessage, "Connection failed");
        Serial.println("[FILESERVER] Connection timeout");
        WiFi.disconnect(true);
        state = FileServerState::IDLE;
    }
}

void FileServer::updateRunning() {
    if (server) {
        server->handleClient();
    }
    
    // Check WiFi connection every 5 seconds
    uint32_t now = millis();
    if (now - lastReconnectCheck > 5000) {
        lastReconnectCheck = now;
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[FILESERVER] WiFi lost, reconnecting...");
            strcpy(statusMessage, "retry hack.");
            
            // Stop server but keep credentials
            if (server) {
                server->stop();
                delete server;
                server = nullptr;
            }
            
            // Stop mDNS before reconnect
            MDNS.end();
            
            // Restart connection
            WiFi.disconnect(true);
            WiFi.begin(targetSSID, targetPassword);
            state = FileServerState::RECONNECTING;
            connectStartTime = millis();
        }
    }
}

uint64_t FileServer::getSDFreeSpace() {
    return SD.totalBytes() - SD.usedBytes();
}

uint64_t FileServer::getSDTotalSpace() {
    return SD.totalBytes();
}

void FileServer::handleRoot() {
    server->send(200, "text/html", HTML_TEMPLATE);
}

void FileServer::handleSDInfo() {
    String json = "{\"total\":";
    json += String((unsigned long)(SD.totalBytes() / 1024));  // KB
    json += ",\"used\":";
    json += String((unsigned long)(SD.usedBytes() / 1024));
    json += ",\"free\":";
    json += String((unsigned long)((SD.totalBytes() - SD.usedBytes()) / 1024));
    json += "}";
    server->send(200, "application/json", json);
}

void FileServer::handleFileList() {
    String dir = server->arg("dir");
    bool full = server->arg("full") == "1";
    if (dir.isEmpty()) dir = "/";
    
    // Security: prevent directory traversal
    if (dir.indexOf("..") >= 0) {
        server->send(400, "application/json", "[]");
        return;
    }
    
    File root = SD.open(dir);
    if (!root || !root.isDirectory()) {
        server->send(200, "application/json", "[]");
        return;
    }
    
    String json = "[";
    bool first = true;
    
    File file = root.openNextFile();
    while (file) {
        if (!first) json += ",";
        first = false;
        
        // Escape filename for JSON
        String fname = file.name();
        fname.replace("\\", "\\\\");
        fname.replace("\"", "\\\"");
        
        json += "{\"name\":\"";
        json += fname;
        json += "\",\"size\":";
        json += String(file.size());
        if (full) {
            json += ",\"isDir\":";
            json += file.isDirectory() ? "true" : "false";
        }
        json += "}";
        
        file.close();
        file = root.openNextFile();
    }
    
    root.close();
    json += "]";
    server->send(200, "application/json", json);
}

void FileServer::handleDownload() {
    String path = server->arg("f");
    String dir = server->arg("dir");  // For ZIP download
    
    // ZIP download of folder
    if (!dir.isEmpty()) {
        // Simple implementation: send files one by one is not possible
        // Instead, we'll create a simple text manifest for now
        // Full ZIP requires external library
        server->send(501, "text/plain", "ZIP download not yet implemented - download files individually");
        return;
    }
    
    if (path.isEmpty()) {
        server->send(400, "text/plain", "Missing file path");
        return;
    }
    
    // Security: prevent directory traversal
    if (path.indexOf("..") >= 0) {
        server->send(400, "text/plain", "Invalid path");
        return;
    }
    
    File file = SD.open(path);
    if (!file || file.isDirectory()) {
        server->send(404, "text/plain", "File not found");
        return;
    }
    
    // Get filename for Content-Disposition
    String filename = path;
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash >= 0) {
        filename = path.substring(lastSlash + 1);
    }
    
    // Determine content type
    String contentType = "application/octet-stream";
    if (path.endsWith(".txt")) contentType = "text/plain";
    else if (path.endsWith(".csv")) contentType = "text/csv";
    else if (path.endsWith(".json")) contentType = "application/json";
    else if (path.endsWith(".pcap")) contentType = "application/vnd.tcpdump.pcap";
    
    server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    server->streamFile(file, contentType);
    file.close();
}

void FileServer::handleUpload() {
    server->send(200, "text/plain", "OK");
}

void FileServer::handleUploadProcess() {
    HTTPUpload& upload = server->upload();
    
    if (upload.status == UPLOAD_FILE_START) {
        uploadDir = server->arg("dir");
        if (uploadDir.isEmpty()) uploadDir = "/";
        if (uploadDir != "/" && !uploadDir.endsWith("/")) uploadDir += "/";
        if (uploadDir == "/") uploadDir = "";  // Root doesn't need slash prefix
        
        // Security: prevent directory traversal
        String filename = upload.filename;
        if (filename.indexOf("..") >= 0 || uploadDir.indexOf("..") >= 0) {
            Serial.println("[FILESERVER] Path traversal attempt blocked");
            return;
        }
        
        String path = uploadDir + "/" + filename;
        if (path.startsWith("//")) path = path.substring(1);
        Serial.printf("[FILESERVER] Upload start: %s\n", path.c_str());
        
        uploadFile = SD.open(path, FILE_WRITE);
        if (!uploadFile) {
            Serial.println("[FILESERVER] Failed to open file for writing");
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            uploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            Serial.printf("[FILESERVER] Upload complete: %u bytes\n", upload.totalSize);
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        // Client disconnected or error - close file to prevent leak
        if (uploadFile) {
            uploadFile.close();
            Serial.println("[FILESERVER] Upload aborted - file handle closed");
        }
    }
}

void FileServer::handleDelete() {
    String path = server->arg("f");
    if (path.isEmpty()) {
        server->send(400, "text/plain", "Missing path");
        return;
    }
    
    // Security: prevent directory traversal
    if (path.indexOf("..") >= 0) {
        server->send(400, "text/plain", "Invalid path");
        return;
    }
    
    // Check if it's a directory
    File f = SD.open(path);
    bool isDir = f && f.isDirectory();
    f.close();
    
    bool success = false;
    if (isDir) {
        // Recursive delete for directories
        success = SD.rmdir(path);
        if (!success) {
            // Try to remove contents first
            File dir = SD.open(path);
            if (dir) {
                File entry = dir.openNextFile();
                while (entry) {
                    String entryPath = path + "/" + String(entry.name());
                    if (entry.isDirectory()) {
                        SD.rmdir(entryPath);
                    } else {
                        SD.remove(entryPath);
                    }
                    entry.close();
                    entry = dir.openNextFile();
                }
                dir.close();
            }
            success = SD.rmdir(path);
        }
    } else {
        success = SD.remove(path);
    }
    
    if (success) {
        server->send(200, "text/plain", "Deleted");
        Serial.printf("[FILESERVER] Deleted: %s\n", path.c_str());
    } else {
        server->send(500, "text/plain", "Delete failed");
    }
}

void FileServer::handleMkdir() {
    String path = server->arg("f");
    if (path.isEmpty()) {
        server->send(400, "text/plain", "Missing path");
        return;
    }
    
    // Security: prevent directory traversal
    if (path.indexOf("..") >= 0) {
        server->send(400, "text/plain", "Invalid path");
        return;
    }
    
    if (SD.mkdir(path)) {
        server->send(200, "text/plain", "Created");
        Serial.printf("[FILESERVER] Created folder: %s\n", path.c_str());
    } else {
        server->send(500, "text/plain", "Create folder failed");
    }
}

void FileServer::handleNotFound() {
    server->send(404, "text/plain", "Not found");
}

const char* FileServer::getHTML() {
    return HTML_TEMPLATE;
}
