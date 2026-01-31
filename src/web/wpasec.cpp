// WPA-SEC distributed cracking service client
// https://wpa-sec.stanev.org/

#include "wpasec.h"
#include "../core/sd_layout.h"
#include "../core/config.h"
#include "../core/wifi_utils.h"
#include "../core/network_recon.h"
#include "../piglet/mood.h"
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctype.h>
#include <esp_heap_caps.h>

// WPA-SEC API
static const char* WPASEC_HOST = "wpa-sec.stanev.org";
static const uint16_t WPASEC_PORT = 443;
static const char* WPASEC_UPLOAD_PATH = "/";
static const char* WPASEC_POTFILE_PATH = "/?api&dl=1";
static const size_t WPASEC_MAX_CACHE_ENTRIES = 500;

// Static member initialization
bool WPASec::cacheLoaded = false;
char WPASec::lastError[64] = "";
std::map<String, WPASec::CacheEntry> WPASec::crackedCache;
std::map<String, bool> WPASec::uploadedCache;
volatile bool WPASec::busy = false;
bool WPASec::batchMode = false;

bool WPASec::isBusy() {
    return busy;
}

String WPASec::normalizeBSSID(const char* bssid) {
    if (!bssid) return "";
    String out = "";
    out.reserve(12);
    for (int i = 0; bssid[i]; i++) {
        char c = bssid[i];
        if (c != ':' && c != '-') {
            out += (char)toupper(c);
        }
    }
    return out;
}

void WPASec::normalizeBSSID_Char(const char* bssid, char* output, size_t outLen) {
    if (!bssid || !output || outLen < 1) return;
    size_t outIdx = 0;
    for (int i = 0; bssid[i] && outIdx < outLen - 1; i++) {
        char c = bssid[i];
        if (c != ':' && c != '-') {
            output[outIdx++] = (char)toupper(c);
        }
    }
    output[outIdx] = '\0';
}

// ============================================================================
// Cache Management (disk only)
// ============================================================================

bool WPASec::loadUploadedList() {
    uploadedCache.clear();
    const char* uploadedPath = SDLayout::wpasecUploadedPath();
    if (!SD.exists(uploadedPath)) return true;

    File f = SD.open(uploadedPath, FILE_READ);
    if (!f) {
        strncpy(lastError, "CANNOT OPEN UPLOADED", sizeof(lastError) - 1);
        lastError[sizeof(lastError) - 1] = '\0';
        return false;
    }

    String line;
    line.reserve(64);  // Pre-allocate to reduce heap fragmentation
    while (f.available() && uploadedCache.size() < WPASEC_MAX_CACHE_ENTRIES) {
        line = f.readStringUntil('\n');  // Reuses existing capacity
        line.trim();
        if (!line.isEmpty()) {
            String key = normalizeBSSID(line.c_str());
            if (!key.isEmpty()) {
                uploadedCache[key] = true;
            }
        }
    }

    f.close();
    return true;
}

bool WPASec::loadCache() {
    if (cacheLoaded) return true;

    crackedCache.clear();
    uploadedCache.clear();

    const char* cachePath = SDLayout::wpasecResultsPath();
    if (SD.exists(cachePath)) {
        File f = SD.open(cachePath, FILE_READ);
        if (!f) {
            strncpy(lastError, "CANNOT OPEN CACHE", sizeof(lastError) - 1);
            lastError[sizeof(lastError) - 1] = '\0';
            return false;
        }

        // Format: AP_BSSID:CLIENT_BSSID:SSID:password (WPA-SEC potfile format)
        // AP_BSSID is always 12 hex chars, CLIENT_BSSID is always 12 hex chars
        // Cap at 500 entries to prevent memory exhaustion
        String line;
        line.reserve(128);  // Pre-allocate to reduce heap fragmentation
        while (f.available() && crackedCache.size() < WPASEC_MAX_CACHE_ENTRIES) {
            line = f.readStringUntil('\n');  // Reuses existing capacity
            line.trim();
            if (line.isEmpty()) continue;

            // WPA-SEC potfile: AP_BSSID:CLIENT_BSSID:SSID:password
            // Both BSSIDs are exactly 12 hex chars (no colons)
            // Password can contain colons, so we must find the THIRD colon
            int firstColon = line.indexOf(':');
            int secondColon = (firstColon > 0) ? line.indexOf(':', firstColon + 1) : -1;
            int thirdColon = (secondColon > 0) ? line.indexOf(':', secondColon + 1) : -1;

            // Validate: AP BSSID at pos 0-11 (colon at 12), client BSSID at pos 13-24 (colon at 25)
            if (firstColon == 12 && secondColon == 25 && thirdColon > secondColon) {
                String bssid = line.substring(0, firstColon);  // AP BSSID
                String ssid = line.substring(secondColon + 1, thirdColon);  // SSID between 2nd and 3rd colon
                String password = line.substring(thirdColon + 1);  // Everything after 3rd colon

                bssid = normalizeBSSID(bssid.c_str());

                CacheEntry entry;
                entry.ssid = ssid;
                entry.password = password;
                crackedCache[bssid] = entry;
            }
        }

        f.close();
    }

    if (!loadUploadedList()) {
        return false;
    }

    cacheLoaded = true;
    return true;
}

// ============================================================================
// Local Cache Queries
// ============================================================================

bool WPASec::isCracked(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    return crackedCache.find(key) != crackedCache.end();
}

String WPASec::getPassword(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    auto it = crackedCache.find(key);
    if (it != crackedCache.end()) {
        return it->second.password;
    }
    return "";
}

String WPASec::getSSID(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    auto it = crackedCache.find(key);
    if (it != crackedCache.end()) {
        return it->second.ssid;
    }
    return "";
}

uint16_t WPASec::getCrackedCount() {
    loadCache();
    return crackedCache.size();
}

bool WPASec::isUploaded(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    if (crackedCache.find(key) != crackedCache.end()) return true;
    return uploadedCache.find(key) != uploadedCache.end();
}

const char* WPASec::getLastError() {
    return lastError;
}

void WPASec::freeCacheMemory() {
    size_t crackedCount = crackedCache.size();
    size_t uploadedCount = uploadedCache.size();
    crackedCache.clear();
    uploadedCache.clear();
    cacheLoaded = false;
    Serial.printf("[WPASEC] Freed cache: %u cracked, %u uploaded\n",
                  (unsigned int)crackedCount, (unsigned int)uploadedCount);
}

bool WPASec::saveUploadedList() {
    const char* uploadedPath = SDLayout::wpasecUploadedPath();
    File f = SD.open(uploadedPath, FILE_WRITE);
    if (!f) {
        strncpy(lastError, "CANNOT WRITE UPLOADED", sizeof(lastError) - 1);
        lastError[sizeof(lastError) - 1] = '\0';
        return false;
    }

    for (const auto& kv : uploadedCache) {
        f.println(kv.first);
    }

    f.close();
    return true;
}

void WPASec::markAsUploaded(const char* bssid) {
    loadCache();
    String key = normalizeBSSID(bssid);
    if (!key.isEmpty()) {
        if (uploadedCache.find(key) == uploadedCache.end() &&
            uploadedCache.size() >= WPASEC_MAX_CACHE_ENTRIES) {
            // Cap in-memory cache to avoid unbounded heap growth.
            return;
        }
        uploadedCache[key] = true;
        if (!batchMode) {
            saveUploadedList();  // Only save immediately if not in batch mode
        }
    }
}

void WPASec::beginBatchUpload() {
    batchMode = true;
}

void WPASec::endBatchUpload() {
    if (batchMode) {
        batchMode = false;
        saveUploadedList();  // Single save at end of batch
        Serial.println("[WPASEC] Batch upload complete, saved uploaded list");
    }
}

// ============================================================================
// Network Operations
// ============================================================================

bool WPASec::hasApiKey() {
    const char* key = Config::wifi().wpaSecKey;
    if (!key || key[0] == '\0') return false;
    // Key should be 32 hex characters
    size_t len = strlen(key);
    if (len != 32) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit(key[i])) return false;
    }
    return true;
}

bool WPASec::canSync() {
    // Free caches to maximize available heap
    freeCacheMemory();
    
    size_t freeHeap = ESP.getFreeHeap();
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    
    Serial.printf("[WPASEC] canSync: %u free, %u contiguous (need %u/%u)\n", 
                  (unsigned int)freeHeap, (unsigned int)largestBlock,
                  (unsigned int)HeapPolicy::kMinHeapForTls,
                  (unsigned int)HeapPolicy::kMinContigForTls);
    
    // Check fragmentation first (more specific error)
    if (largestBlock < HeapPolicy::kMinContigForTls) {
        snprintf(lastError, sizeof(lastError), "FRAGMENTED: %uKB", 
                 (unsigned int)(largestBlock / 1024));
        return false;
    }
    
    // Then total heap
    if (freeHeap < HeapPolicy::kMinHeapForTls) {
        snprintf(lastError, sizeof(lastError), "LOW HEAP: %uKB", 
                 (unsigned int)(freeHeap / 1024));
        return false;
    }
    
    return true;
}

bool WPASec::uploadSingleCapture(const char* filepath, const char* bssid) {
    if (!filepath || !bssid) return false;
    
    Serial.printf("[WPASEC] Uploading: %s\n", filepath);
    
    // Check file exists and get size
    File capFile = SD.open(filepath, FILE_READ);
    if (!capFile) {
        Serial.printf("[WPASEC] Cannot open file: %s\n", filepath);
        return false;
    }
    size_t fileSize = capFile.size();
    if (fileSize == 0 || fileSize > 100000) {  // Max 100KB
        capFile.close();
        Serial.printf("[WPASEC] Invalid file size: %u\n", (unsigned int)fileSize);
        return false;
    }
    
    // Extract filename from path
    const char* filename = strrchr(filepath, '/');
    filename = filename ? filename + 1 : filepath;
    
    // Create WiFiClientSecure with minimal buffers
    WiFiClientSecure client;
    client.setInsecure();  // Skip cert validation - saves ~10KB heap
    
    // Connect with timeout
    Serial.printf("[WPASEC] Connecting to %s:%d\n", WPASEC_HOST, WPASEC_PORT);
    if (!client.connect(WPASEC_HOST, WPASEC_PORT, 10000)) {
        capFile.close();
        strncpy(lastError, "TLS CONNECT FAILED", sizeof(lastError) - 1);
        Serial.println("[WPASEC] TLS connection failed");
        return false;
    }
    
    // Build multipart boundary
    char boundary[32];
    snprintf(boundary, sizeof(boundary), "----WPASec%08lX", millis());
    
    // Calculate content length
    // Multipart format:
    // --boundary\r\n
    // Content-Disposition: form-data; name="file"; filename="xxx"\r\n
    // Content-Type: application/octet-stream\r\n\r\n
    // <file data>
    // \r\n--boundary--\r\n
    char disposition[128];
    snprintf(disposition, sizeof(disposition),
             "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"",
             filename);
    
    size_t contentLength = 2 + strlen(boundary) + 2 +           // --boundary\r\n
                           strlen(disposition) + 2 +             // disposition\r\n
                           36 + 4 +                              // Content-Type + \r\n\r\n
                           fileSize +                            // file data
                           2 + 2 + strlen(boundary) + 4;         // \r\n--boundary--\r\n
    
    // Send HTTP headers
    client.printf("POST %s HTTP/1.1\r\n", WPASEC_UPLOAD_PATH);
    client.printf("Host: %s\r\n", WPASEC_HOST);
    client.printf("Cookie: key=%s\r\n", Config::wifi().wpaSecKey);
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    client.printf("Content-Length: %u\r\n", (unsigned int)contentLength);
    client.print("Connection: close\r\n\r\n");
    
    // Send multipart body
    client.printf("--%s\r\n", boundary);
    client.printf("%s\r\n", disposition);
    client.print("Content-Type: application/octet-stream\r\n\r\n");
    
    // Stream file in chunks (heap-safe)
    char chunk[256];
    size_t sent = 0;
    while (capFile.available() && sent < fileSize) {
        size_t toRead = min((size_t)sizeof(chunk), fileSize - sent);
        size_t bytesRead = capFile.read((uint8_t*)chunk, toRead);
        if (bytesRead > 0) {
            client.write((uint8_t*)chunk, bytesRead);
            sent += bytesRead;
        }
        yield();  // Let WiFi stack breathe
    }
    capFile.close();
    
    // End multipart
    client.printf("\r\n--%s--\r\n", boundary);
    
    // Read response (just check status code)
    unsigned long timeout = millis() + 10000;
    while (client.connected() && !client.available() && millis() < timeout) {
        delay(10);
        yield();  // Prevent WDT during response wait
    }
    
    bool success = false;
    if (client.available()) {
        char response[64];
        size_t len = client.readBytesUntil('\n', response, sizeof(response) - 1);
        response[len] = '\0';
        Serial.printf("[WPASEC] Response: %s\n", response);
        
        // HTTP/1.1 200 OK or similar success
        if (strstr(response, "200") || strstr(response, "201")) {
            success = true;
        } else if (strstr(response, "409")) {
            // Already uploaded - treat as success
            success = true;
            Serial.println("[WPASEC] Already uploaded (409)");
        }
    }
    
    client.stop();
    
    if (success) {
        // NOTE: Don't mark uploaded here - caller handles marking after all TLS operations
        // This avoids reloading cache during TLS when heap is tight
        Serial.printf("[WPASEC] Upload success: %s\n", bssid);
    } else {
        strncpy(lastError, "UPLOAD REJECTED", sizeof(lastError) - 1);
    }
    
    return success;
}

bool WPASec::downloadPotfile(uint16_t& newCracks) {
    newCracks = 0;
    
    Serial.println("[WPASEC] Downloading potfile...");
    
    // Create WiFiClientSecure with minimal buffers
    WiFiClientSecure client;
    client.setInsecure();
    
    if (!client.connect(WPASEC_HOST, WPASEC_PORT, 10000)) {
        strncpy(lastError, "POTFILE TLS FAILED", sizeof(lastError) - 1);
        Serial.println("[WPASEC] Potfile TLS connection failed");
        return false;
    }
    
    // Send GET request
    client.printf("GET %s HTTP/1.1\r\n", WPASEC_POTFILE_PATH);
    client.printf("Host: %s\r\n", WPASEC_HOST);
    client.printf("Cookie: key=%s\r\n", Config::wifi().wpaSecKey);
    client.print("Connection: close\r\n\r\n");
    
    // Wait for response
    unsigned long timeout = millis() + 15000;
    while (client.connected() && !client.available() && millis() < timeout) {
        delay(10);
        yield();  // Prevent WDT during response wait
    }
    
    if (!client.available()) {
        client.stop();
        strncpy(lastError, "POTFILE TIMEOUT", sizeof(lastError) - 1);
        return false;
    }
    
    // Skip HTTP headers
    bool headersEnded = false;
    char headerLine[128];
    while (client.connected() && client.available() && !headersEnded) {
        size_t len = client.readBytesUntil('\n', headerLine, sizeof(headerLine) - 1);
        headerLine[len] = '\0';
        // Empty line marks end of headers
        if (len <= 1 || (len == 1 && headerLine[0] == '\r')) {
            headersEnded = true;
        }
    }
    
    if (!headersEnded) {
        client.stop();
        strncpy(lastError, "POTFILE BAD RESPONSE", sizeof(lastError) - 1);
        return false;
    }
    
    // Open cache file for writing (overwrite)
    const char* cachePath = SDLayout::wpasecResultsPath();
    File cacheFile = SD.open(cachePath, FILE_WRITE);
    if (!cacheFile) {
        client.stop();
        strncpy(lastError, "CANNOT WRITE CACHE", sizeof(lastError) - 1);
        return false;
    }
    
    // Stream potfile line-by-line directly to SD
    // Format: BSSID:SSID:password (hashcat potfile format)
    char lineBuf[160];  // Should be enough for BSSID:SSID:password
    uint16_t lineCount = 0;
    
    while (client.connected() || client.available()) {
        if (client.available()) {
            size_t len = client.readBytesUntil('\n', lineBuf, sizeof(lineBuf) - 1);
            if (len > 0) {
                lineBuf[len] = '\0';
                // Trim \r if present
                if (len > 0 && lineBuf[len - 1] == '\r') {
                    lineBuf[len - 1] = '\0';
                }
                
                // Validate line has at least 2 colons (BSSID:SSID:password)
                int colonCount = 0;
                for (size_t i = 0; lineBuf[i]; i++) {
                    if (lineBuf[i] == ':') colonCount++;
                }
                
                if (colonCount >= 2 && strlen(lineBuf) > 10) {
                    cacheFile.println(lineBuf);
                    lineCount++;
                }
            }
        } else {
            delay(10);
        }
        
        // Safety timeout
        if (millis() > timeout + 30000) {
            Serial.println("[WPASEC] Potfile download timeout");
            break;
        }
        
        yield();
    }
    
    cacheFile.close();
    client.stop();
    
    Serial.printf("[WPASEC] Potfile downloaded: %u entries\n", (unsigned int)lineCount);
    newCracks = lineCount;
    
    return true;
}

WPASecSyncResult WPASec::syncCaptures(WPASecProgressCallback cb) {
    WPASecSyncResult result = {};
    result.success = false;
    result.error[0] = '\0';
    
    busy = true;
    
    // Pause NetworkRecon - TLS operations conflict with promiscuous mode
    // conditionHeapForTLS() overrides promiscuous callbacks, breaking NetworkRecon state
    bool wasReconRunning = NetworkRecon::isRunning();
    if (wasReconRunning) {
        Serial.println("[WPASEC] Pausing NetworkRecon for TLS operations");
        NetworkRecon::pause();
    }
    
    // Pre-flight checks
    if (!hasApiKey()) {
        strncpy(result.error, "NO WPA-SEC KEY", sizeof(result.error) - 1);
        if (wasReconRunning) NetworkRecon::resume();
        busy = false;
        return result;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        strncpy(result.error, "WIFI NOT CONNECTED", sizeof(result.error) - 1);
        if (wasReconRunning) NetworkRecon::resume();
        busy = false;
        return result;
    }
    
    // Proactive heap conditioning - condition early when heap is marginal
    // This prevents fragmentation from getting critical before TLS attempts
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (largestBlock < HeapPolicy::kProactiveTlsConditioning &&
        largestBlock >= HeapPolicy::kMinContigForTls) {
        if (cb) {
            cb("OPTIMIZING HEAP", 0, 0);
        }
        Serial.printf("[WPASEC] Proactive conditioning: %u < %u threshold\n",
                      (unsigned int)largestBlock,
                      (unsigned int)HeapPolicy::kProactiveTlsConditioning);
        WiFiUtils::conditionHeapForTLS();
    }
    
    // Check if heap is sufficient for TLS operations
    if (!canSync()) {
        // Heap insufficient - try "OINK bounce" conditioning
        // This reclaims BLE memory and coalesces fragmented heap blocks
        if (cb) {
            cb("CONDITIONING HEAP", 0, 0);
        }
        Serial.println("[WPASEC] Heap insufficient, attempting conditioning...");
        
        size_t largestAfter = WiFiUtils::conditionHeapForTLS();
        
        // Check again after conditioning
        if (!canSync()) {
            // Still insufficient - notify user via speech balloon
            Mood::setStatusMessage("HEAP TIGHT - TRY OINK");
            snprintf(result.error, sizeof(result.error), 
                     "%s (TRY OINK)", lastError);
            if (wasReconRunning) NetworkRecon::resume();
            busy = false;
            return result;
        }
        
        Serial.printf("[WPASEC] Conditioning successful: largest=%u\n", 
                      (unsigned int)largestAfter);
    }
    
    // Collect files to upload from handshakes directory
    const char* hsDir = SDLayout::handshakesDir();
    if (!SD.exists(hsDir)) {
        strncpy(result.error, "NO HANDSHAKES DIR", sizeof(result.error) - 1);
        if (wasReconRunning) NetworkRecon::resume();
        busy = false;
        return result;
    }
    
    // First pass: count files and check which need upload
    // We need to reload cache for this check
    loadCache();
    
    // Collect pending uploads (store paths temporarily)
    struct PendingUpload {
        char path[80];
        char bssid[13];
    };
    static PendingUpload pendingUploads[50];  // Max 50 per sync
    uint8_t pendingCount = 0;
    
    File dir = SD.open(hsDir);
    if (dir && dir.isDirectory()) {
        File file = dir.openNextFile();
        uint8_t filesScanned = 0;
        while (file && pendingCount < 50) {
            // Yield every 10 files to prevent WDT on large directories
            if (++filesScanned >= 10) {
                filesScanned = 0;
                yield();
            }
            
            const char* fname = file.name();
            size_t fnameLen = strlen(fname);
            
            // Check extension without String allocation
            bool isPCAP = (fnameLen > 5 && strcmp(fname + fnameLen - 5, ".pcap") == 0);
            bool is22000 = (fnameLen > 6 && strcmp(fname + fnameLen - 6, ".22000") == 0);
            
            if (isPCAP || is22000) {
                // Extract BSSID from filename (first 12 chars before extension)
                // Find the dot position
                const char* dot = strrchr(fname, '.');
                size_t baseLen = dot ? (size_t)(dot - fname) : fnameLen;
                
                // Check for _hs suffix
                if (baseLen > 3 && strncmp(fname + baseLen - 3, "_hs", 3) == 0) {
                    baseLen -= 3;
                }
                
                if (baseLen >= 12) {
                    char bssid[13];
                    memcpy(bssid, fname, 12);
                    bssid[12] = '\0';
                    
                    // Check if already uploaded or cracked
                    if (!isUploaded(bssid)) {
                        snprintf(pendingUploads[pendingCount].path, 
                                sizeof(pendingUploads[pendingCount].path),
                                "%s/%s", hsDir, fname);
                        memcpy(pendingUploads[pendingCount].bssid, bssid, 13);
                        pendingCount++;
                    } else {
                        result.skipped++;
                    }
                }
            }
            file.close();
            file = dir.openNextFile();
        }
        dir.close();
    }
    
    Serial.printf("[WPASEC] Found %u files to upload, %u skipped\n", 
                  (unsigned int)pendingCount, (unsigned int)result.skipped);
    
    // Free cache before TLS operations - keeps heap clear for WiFiClientSecure
    freeCacheMemory();
    
    // Track successful uploads with bitmask - avoids reloading cache during TLS
    // We mark uploaded AFTER all TLS operations complete to keep heap clear
    uint8_t successMask[50] = {0};
    
    // Upload each pending file
    for (uint8_t i = 0; i < pendingCount; i++) {
        if (cb) {
            char status[32];
            snprintf(status, sizeof(status), "UPLOAD %u/%u", i + 1, pendingCount);
            cb(status, i + 1, pendingCount);
        }
        
        Serial.printf("[WPASEC] Heap before upload %u: %u\n", 
                      i, (unsigned int)ESP.getFreeHeap());
        
        if (uploadSingleCapture(pendingUploads[i].path, pendingUploads[i].bssid)) {
            result.uploaded++;
            successMask[i] = 1;  // Track for deferred marking
        } else {
            result.failed++;
            Serial.printf("[WPASEC] Failed: %s\n", pendingUploads[i].path);
        }
        
        // Small delay between uploads to let heap settle
        delay(100);
        yield();
    }
    
    // Mark successful uploads AFTER all TLS operations complete
    // This avoids cache reload during TLS when heap is tight
    if (result.uploaded > 0) {
        loadCache();
        for (uint8_t i = 0; i < pendingCount; i++) {
            if (successMask[i]) {
                String key = normalizeBSSID(pendingUploads[i].bssid);
                uploadedCache[key] = true;
            }
        }
        saveUploadedList();
        Serial.printf("[WPASEC] Marked %u uploads after TLS complete\n", result.uploaded);
    }
    
    // Download potfile
    if (cb) {
        cb("DOWNLOADING POTFILE", 0, 0);
    }
    
    // Free any residual memory before potfile TLS
    // NOTE: We do NOT recondition heap mid-sync - that causes more fragmentation!
    // If heap was good enough to start sync, trust it. Graceful degradation if not.
    freeCacheMemory();
    delay(100);
    
    Serial.printf("[WPASEC] Heap before potfile: %u largest=%u\n", 
                  (unsigned int)ESP.getFreeHeap(),
                  (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    uint16_t newCracks = 0;
    bool potfileOk = false;
    
    // Attempt potfile if heap is sufficient - no reconditioning, graceful skip if low
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= HeapPolicy::kMinContigForTls) {
        potfileOk = downloadPotfile(newCracks);
        if (potfileOk) {
            result.newCracked = newCracks;
            // Reload cache to get cracked count
            loadCache();
            result.cracked = crackedCache.size();
        }
    } else {
        Serial.printf("[WPASEC] Skipping potfile: insufficient heap (%u < %u)\n",
                      (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                      (unsigned int)HeapPolicy::kMinContigForTls);
        snprintf(lastError, sizeof(lastError), "POTFILE SKIP: LOW HEAP");
    }
    
    // Graceful degradation: partial success if uploads worked but potfile failed
    if (!potfileOk && result.uploaded > 0) {
        // Uploads succeeded, potfile failed - still report partial success
        snprintf(result.error, sizeof(result.error), "POTFILE: %s", lastError);
        result.success = true;  // Partial success - uploads worked
    } else if (!potfileOk) {
        strncpy(result.error, lastError, sizeof(result.error) - 1);
        result.success = (result.failed == 0);
    } else {
        result.success = (result.failed == 0);
    }
    
    // Resume NetworkRecon after sync operations complete
    if (wasReconRunning) {
        Serial.println("[WPASEC] Resuming NetworkRecon after TLS operations");
        NetworkRecon::resume();
    }
    
    busy = false;
    Serial.printf("[WPASEC] Sync complete: uploaded=%u failed=%u cracked=%u\n",
                  (unsigned int)result.uploaded, (unsigned int)result.failed,
                  (unsigned int)result.cracked);
    
    return result;
}
