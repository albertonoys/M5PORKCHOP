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

// Black & white HTML interface with full filesystem navigation
static const char HTML_TEMPLATE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PORKCHOP File Manager</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { 
            background: #000; 
            color: #fff; 
            font-family: 'Courier New', monospace;
            padding: 20px;
            max-width: 900px;
            margin: 0 auto;
        }
        h1 { 
            border-bottom: 2px solid #fff; 
            padding-bottom: 10px; 
            margin-bottom: 10px;
            font-size: 1.5em;
        }
        .sd-info {
            color: #888;
            margin-bottom: 15px;
            font-size: 0.9em;
        }
        .breadcrumb {
            margin: 10px 0;
            padding: 8px;
            background: #111;
            border: 1px solid #333;
        }
        .breadcrumb a { color: #fff; text-decoration: none; }
        .breadcrumb a:hover { text-decoration: underline; }
        .file-list {
            border: 1px solid #444;
            margin: 10px 0;
        }
        .file-item { 
            display: flex; 
            justify-content: space-between; 
            align-items: center;
            padding: 10px;
            border-bottom: 1px solid #333;
        }
        .file-item:hover { background: #111; }
        .file-item:last-child { border-bottom: none; }
        .file-icon { margin-right: 10px; }
        .file-name { flex: 1; cursor: pointer; }
        .file-name a { color: #fff; text-decoration: none; }
        .file-name a:hover { text-decoration: underline; }
        .file-size { color: #888; margin: 0 15px; min-width: 80px; text-align: right; }
        .btn {
            background: #fff;
            color: #000;
            border: none;
            padding: 5px 12px;
            cursor: pointer;
            font-family: inherit;
            font-size: 0.9em;
            margin-left: 5px;
        }
        .btn:hover { background: #ccc; }
        .btn-del { background: #333; color: #fff; border: 1px solid #fff; }
        .btn-del:hover { background: #500; }
        .btn-small { padding: 3px 8px; font-size: 0.8em; }
        .toolbar {
            display: flex;
            gap: 10px;
            margin: 15px 0;
            flex-wrap: wrap;
        }
        .upload-section {
            padding: 15px;
            border: 1px solid #fff;
            margin-top: 15px;
        }
        .upload-section input[type="file"] { 
            margin: 10px 0;
            color: #fff;
        }
        .progress-bar {
            width: 100%;
            height: 20px;
            background: #333;
            margin-top: 10px;
            display: none;
        }
        .progress-fill {
            height: 100%;
            background: #fff;
            width: 0%;
            transition: width 0.2s;
        }
        .status { 
            color: #888; 
            margin-top: 15px; 
            font-size: 0.9em;
        }
        input[type="text"] {
            background: #000;
            color: #fff;
            border: 1px solid #fff;
            padding: 5px 10px;
            font-family: inherit;
        }
        .modal {
            display: none;
            position: fixed;
            top: 0; left: 0;
            width: 100%; height: 100%;
            background: rgba(0,0,0,0.8);
            justify-content: center;
            align-items: center;
        }
        .modal-content {
            background: #000;
            border: 2px solid #fff;
            padding: 20px;
            max-width: 400px;
        }
        .modal-content h3 { margin-bottom: 15px; }
        .modal-content input { width: 100%; margin: 10px 0; }
    </style>
</head>
<body>
    <h1>PORKCHOP File Manager</h1>
    <div class="sd-info" id="sdInfo">Loading SD info...</div>
    
    <div class="breadcrumb" id="breadcrumb"></div>
    
    <div class="toolbar">
        <button class="btn" onclick="loadDir(currentPath)">Refresh</button>
        <button class="btn" onclick="showNewFolderModal()">New Folder</button>
        <button class="btn" onclick="downloadAll()">Download All (ZIP)</button>
    </div>
    
    <div class="file-list" id="fileList"></div>
    
    <div class="upload-section">
        <strong>Upload to current folder</strong>
        <form id="uploadForm" enctype="multipart/form-data">
            <input type="file" id="fileInput" name="file" multiple>
            <button type="submit" class="btn">Upload</button>
        </form>
        <div class="progress-bar" id="progressBar">
            <div class="progress-fill" id="progressFill"></div>
        </div>
    </div>
    
    <div class="status" id="status">Ready</div>
    
    <!-- New Folder Modal -->
    <div class="modal" id="newFolderModal">
        <div class="modal-content">
            <h3>Create New Folder</h3>
            <input type="text" id="newFolderName" placeholder="Folder name">
            <div style="margin-top: 15px;">
                <button class="btn" onclick="createFolder()">Create</button>
                <button class="btn btn-del" onclick="hideModal()">Cancel</button>
            </div>
        </div>
    </div>
    
    <script>
        let currentPath = '/';
        
        async function loadSDInfo() {
            try {
                const resp = await fetch('/api/sdinfo');
                const info = await resp.json();
                document.getElementById('sdInfo').textContent = 
                    'SD Card: ' + formatSize(info.used) + ' used / ' + formatSize(info.total) + ' total (' + 
                    formatSize(info.free) + ' free)';
            } catch(e) {
                document.getElementById('sdInfo').textContent = 'SD info unavailable';
            }
        }
        
        function updateBreadcrumb() {
            const parts = currentPath.split('/').filter(p => p);
            let html = '<a href="#" onclick="loadDir(\'/\');return false;">/root</a>';
            let path = '';
            for (const p of parts) {
                path += '/' + p;
                const safePath = path;
                html += ' / <a href="#" onclick="loadDir(\'' + safePath + '\');return false;">' + p + '</a>';
            }
            document.getElementById('breadcrumb').innerHTML = html;
        }
        
        async function loadDir(path) {
            currentPath = path || '/';
            updateBreadcrumb();
            
            const container = document.getElementById('fileList');
            container.innerHTML = '<div class="file-item">Loading...</div>';
            
            try {
                const resp = await fetch('/api/ls?dir=' + encodeURIComponent(currentPath) + '&full=1');
                const items = await resp.json();
                
                let html = '';
                
                // Parent directory link
                if (currentPath !== '/') {
                    const parent = currentPath.substring(0, currentPath.lastIndexOf('/')) || '/';
                    html += '<div class="file-item">';
                    html += '<span class="file-icon">[..]</span>';
                    html += '<span class="file-name"><a href="#" onclick="loadDir(\'' + parent + '\');return false;">..</a></span>';
                    html += '<span class="file-size"></span>';
                    html += '</div>';
                }
                
                // Folders first
                for (const item of items.filter(i => i.isDir)) {
                    const itemPath = (currentPath === '/' ? '' : currentPath) + '/' + item.name;
                    html += '<div class="file-item">';
                    html += '<span class="file-icon">[D]</span>';
                    html += '<span class="file-name"><a href="#" onclick="loadDir(\'' + itemPath + '\');return false;">' + item.name + '/</a></span>';
                    html += '<span class="file-size">-</span>';
                    html += '<button class="btn btn-del btn-small" onclick="del(\'' + itemPath + '\', true)">X</button>';
                    html += '</div>';
                }
                
                // Then files
                for (const item of items.filter(i => !i.isDir)) {
                    const itemPath = (currentPath === '/' ? '' : currentPath) + '/' + item.name;
                    html += '<div class="file-item">';
                    html += '<span class="file-icon">[F]</span>';
                    html += '<span class="file-name">' + item.name + '</span>';
                    html += '<span class="file-size">' + formatSize(item.size) + '</span>';
                    html += '<button class="btn btn-small" onclick="download(\'' + itemPath + '\')">DL</button>';
                    html += '<button class="btn btn-del btn-small" onclick="del(\'' + itemPath + '\', false)">X</button>';
                    html += '</div>';
                }
                
                container.innerHTML = html || '<div class="file-item">Empty folder</div>';
            } catch (e) {
                container.innerHTML = '<div class="file-item">Error loading directory</div>';
            }
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
        
        async function downloadAll() {
            document.getElementById('status').textContent = 'Preparing ZIP...';
            window.location.href = '/downloadzip?dir=' + encodeURIComponent(currentPath);
            setTimeout(() => {
                document.getElementById('status').textContent = 'ZIP download started';
            }, 1000);
        }
        
        async function del(path, isDir) {
            const msg = isDir ? 'Delete folder ' + path + ' and all contents?' : 'Delete ' + path + '?';
            if (!confirm(msg)) return;
            
            const endpoint = isDir ? '/rmdir' : '/delete';
            const resp = await fetch(endpoint + '?f=' + encodeURIComponent(path));
            if (resp.ok) {
                document.getElementById('status').textContent = 'Deleted: ' + path;
                loadDir(currentPath);
            } else {
                document.getElementById('status').textContent = 'Delete failed';
            }
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
                document.getElementById('status').textContent = 'Created: ' + path;
                hideModal();
                loadDir(currentPath);
            } else {
                document.getElementById('status').textContent = 'Create folder failed';
            }
        }
        
        document.getElementById('uploadForm').onsubmit = async function(e) {
            e.preventDefault();
            const fileInput = document.getElementById('fileInput');
            
            if (!fileInput.files.length) {
                alert('Select file(s) first');
                return;
            }
            
            const progressBar = document.getElementById('progressBar');
            const progressFill = document.getElementById('progressFill');
            progressBar.style.display = 'block';
            progressFill.style.width = '0%';
            
            for (let i = 0; i < fileInput.files.length; i++) {
                const file = fileInput.files[i];
                document.getElementById('status').textContent = 'Uploading ' + (i+1) + '/' + fileInput.files.length + ': ' + file.name;
                
                const formData = new FormData();
                formData.append('file', file);
                
                try {
                    const xhr = new XMLHttpRequest();
                    
                    await new Promise((resolve, reject) => {
                        xhr.upload.onprogress = function(e) {
                            if (e.lengthComputable) {
                                const pct = (e.loaded / e.total * 100);
                                progressFill.style.width = pct + '%';
                            }
                        };
                        xhr.onload = function() {
                            if (xhr.status === 200) resolve();
                            else reject(new Error('Upload failed'));
                        };
                        xhr.onerror = reject;
                        xhr.open('POST', '/upload?dir=' + encodeURIComponent(currentPath));
                        xhr.send(formData);
                    });
                } catch (e) {
                    document.getElementById('status').textContent = 'Upload error: ' + e.message;
                    progressBar.style.display = 'none';
                    return;
                }
            }
            
            progressBar.style.display = 'none';
            document.getElementById('status').textContent = 'Upload complete!';
            fileInput.value = '';
            loadDir(currentPath);
        };
        
        // Handle Enter key in modal
        document.getElementById('newFolderName').onkeydown = function(e) {
            if (e.key === 'Enter') createFolder();
            if (e.key === 'Escape') hideModal();
        };
        
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
    
    strcpy(statusMessage, "Connecting...");
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
    snprintf(statusMessage, sizeof(statusMessage), "Connecting%.*s", dots, "...");
    
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
            strcpy(statusMessage, "Reconnecting...");
            
            // Stop server but keep credentials
            if (server) {
                server->stop();
                delete server;
                server = nullptr;
            }
            
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

// File upload state
static File uploadFile;
static String uploadDir;

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
