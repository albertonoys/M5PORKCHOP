// WPA-SEC distributed cracking service client implementation (lean, no HTTPClient)

#include "wpasec.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../ui/display.h"
#include "../core/xp.h"
#include "../core/wifi_utils.h"
#include "../core/scope_resume.h"

// Static member initialization
bool WPASec::cacheLoaded = false;
char WPASec::lastError[64] = "";
char WPASec::statusMessage[64] = "READY";
std::map<String, WPASec::CacheEntry> WPASec::crackedCache;
std::map<String, bool> WPASec::uploadedCache;
bool WPASec::busy = false;

// Lightweight heap logger for diagnostics
static void logHeap(const char* tag) {
    Serial.printf("[WPASEC][HEAP] %s free=%lu largest=%lu\n",
        tag,
        (unsigned long)ESP.getFreeHeap(),
        (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static bool shouldAwardSmokedBacon() {
    uint8_t chance = 3;  // base 3%
    if (XP::getClass() == PorkClass::B4C0NM4NC3R) {
        chance += 1;  // +1% for Baconmancer
    }
    return (esp_random() % 100) < chance;
}

// Busy scope guard
class BusyScope {
public:
    explicit BusyScope(bool& ref) : ref_(ref) { ref_ = true; }
    ~BusyScope() { ref_ = false; }
private:
    bool& ref_;
};

// Read and parse HTTP status code from a client response.
// Handles the occasional "HTTP/1.1 100 Continue" prelude.
static int readHttpStatus(WiFiClientSecure& client) {
    for (int guard = 0; guard < 6; ++guard) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (!line.startsWith("HTTP/")) {
            continue;
        }

        // Parse: HTTP/1.1 200 OK
        int sp1 = line.indexOf(' ');
        if (sp1 < 0) return -1;
        int sp2 = line.indexOf(' ', sp1 + 1);
        String codeStr = (sp2 > sp1) ? line.substring(sp1 + 1, sp2) : line.substring(sp1 + 1);
        int code = codeStr.toInt();

        if (code == 100) {
            // Consume remaining headers for the 100 response, then read next status.
            while (client.connected()) {
                String h = client.readStringUntil('\n');
                if (h == "\r" || h.length() == 0) break;
            }
            continue;
        }
        return code;
    }
    return -1;
}

// Heap-safe file scan for a normalized BSSID key (upper, no separators).
static bool fileContainsKey(const char* path, const String& normKey, size_t maxLines = 600) {
    if (!SD.exists(path)) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    size_t lines = 0;
    while (f.available() && lines++ < maxLines) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) continue;

        // Uploaded list stores raw BSSID
        if (line.equalsIgnoreCase(normKey)) {
            f.close();
            return true;
        }

        // Results store "BSSID:SSID:PASS"; match prefix before first ':'
        int c = line.indexOf(':');
        if (c > 0) {
            String prefix = line.substring(0, c);
            prefix.trim();
            if (prefix.equalsIgnoreCase(normKey)) {
                f.close();
                return true;
            }
        }
    }
    f.close();
    return false;
}

void WPASec::init() {
    cacheLoaded = false;
    crackedCache.clear();
    uploadedCache.clear();
    strcpy(lastError, "");
    strcpy(statusMessage, "READY");
}

// ============================================================================
// WiFi Connection (Standalone)
// ============================================================================

bool WPASec::connect() {
    if (WiFi.status() == WL_CONNECTED) {
        strcpy(statusMessage, "ALREADY CONNECTED");
        return true;
    }

    String ssid = Config::wifi().otaSSID;
    String password = Config::wifi().otaPassword;

    if (ssid.isEmpty()) {
        strcpy(lastError, "NO WIFI SSID CONFIGURED");
        strcpy(statusMessage, "NO WIFI SSID");
        return false;
    }

    strcpy(statusMessage, "CONNECTING...");
    Serial.printf("[WPASEC] Connecting to %s\n", ssid.c_str());
    logHeap("connect start");

    WiFiUtils::stopPromiscuous();
    Display::requestSuspendSprites();
    freeCacheMemory();
    Display::waitForSpritesSuspended(2000);

    // Cardputer w/ no PSRAM: never fully power WiFi off.
    // This avoids RX buffer allocation failures and heap fragmentation.
    WiFiUtils::hardReset();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    // Wait for connection with timeout
    uint32_t startTime = millis();
    // Slightly longer timeout helps with phone tether / crowded 2.4GHz.
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        snprintf(statusMessage, sizeof(statusMessage), "IP: %s", WiFi.localIP().toString().c_str());
        Serial.printf("[WPASEC] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        Display::requestResumeSprites();
        logHeap("connect ok");
        return true;
    }

    strcpy(lastError, "CONNECTION TIMEOUT");
    strcpy(statusMessage, "CONNECT FAILED");
    Serial.println("[WPASEC] Connection failed");
    WiFiUtils::shutdown();
    Display::requestResumeSprites();
    logHeap("connect fail");
    return false;
}

void WPASec::disconnect() {
    // Soft shutdown (keeps driver alive) for no-PSRAM stability.
    WiFiUtils::shutdown();
    strcpy(statusMessage, "DISCONNECTED");
    Serial.println("[WPASEC] Disconnected");
}

bool WPASec::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool WPASec::isBusy() {
    return busy;
}

// ============================================================================
// BSSID Normalization
// ============================================================================

String WPASec::normalizeBSSID(const char* bssid) {
    // Remove colons/dashes, convert to uppercase
    String result;
    for (int i = 0; bssid[i]; i++) {
        char c = bssid[i];
        if (c != ':' && c != '-') {
            result += (char)toupper(c);
        }
    }
    return result;
}

// ============================================================================
// Cache Management
// ============================================================================

bool WPASec::loadCache() {
    if (cacheLoaded) return true;

    crackedCache.clear();

    if (!SD.exists(CACHE_FILE)) {
        cacheLoaded = true;
        return true;  // No cache yet, that's OK
    }

    File f = SD.open(CACHE_FILE, FILE_READ);
    if (!f) {
        strcpy(lastError, "CANNOT OPEN CACHE");
        return false;
    }

    // Format: BSSID:SSID:password (one per line)
    // Cap at 500 entries to prevent memory exhaustion
    while (f.available() && crackedCache.size() < 500) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) continue;

        int firstColon = line.indexOf(':');
        int lastColon = line.lastIndexOf(':');

        if (firstColon > 0 && lastColon > firstColon) {
            String bssid = line.substring(0, firstColon);
            String ssid = line.substring(firstColon + 1, lastColon);
            String password = line.substring(lastColon + 1);

            bssid = normalizeBSSID(bssid.c_str());

            CacheEntry entry;
            entry.ssid = ssid;
            entry.password = password;
            crackedCache[bssid] = entry;
        }
    }

    f.close();
    cacheLoaded = true;

    // Also load uploaded list
    loadUploadedList();

    Serial.printf("[WPASEC] Cache loaded: %d cracked, %d uploaded\n",
                  crackedCache.size(), uploadedCache.size());
    return true;
}

bool WPASec::saveCache() {
    File f = SD.open(CACHE_FILE, FILE_WRITE);
    if (!f) {
        strcpy(lastError, "CANNOT WRITE CACHE");
        return false;
    }

    for (auto& pair : crackedCache) {
        f.printf("%s:%s:%s\n", pair.first.c_str(),
                 pair.second.ssid.c_str(), pair.second.password.c_str());
    }

    f.close();
    return true;
}

bool WPASec::loadUploadedList() {
    uploadedCache.clear();

    if (!SD.exists(UPLOADED_FILE)) return true;

    File f = SD.open(UPLOADED_FILE, FILE_READ);
    if (!f) return false;

    // Cap at 500 entries to prevent memory exhaustion
    while (f.available() && uploadedCache.size() < 500) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.isEmpty()) {
            uploadedCache[normalizeBSSID(line.c_str())] = true;
        }
    }

    f.close();
    return true;
}

bool WPASec::saveUploadedList() {
    File f = SD.open(UPLOADED_FILE, FILE_WRITE);
    if (!f) return false;

    for (auto& pair : uploadedCache) {
        f.println(pair.first);
    }

    f.close();
    return true;
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
    // Cracked implies uploaded
    if (crackedCache.find(key) != crackedCache.end()) return true;
    return uploadedCache.find(key) != uploadedCache.end();
}

void WPASec::markUploaded(const char* bssid) {
    String key = normalizeBSSID(bssid);

    // Update in-memory cache only if already loaded (avoid heap spikes).
    if (cacheLoaded) {
        uploadedCache[key] = true;
    }

    // Persist by appending to SD (cheap, streaming, no full rewrite).
    // Guard against duplicates.
    if (fileContainsKey(UPLOADED_FILE, key)) return;

    File f = SD.open(UPLOADED_FILE, FILE_APPEND);
    if (f) {
        f.println(key);
        f.close();
    }
}

// ============================================================================
// API Operations (lean HTTPClient)
// ============================================================================
bool WPASec::fetchResults() {
    if (!isConnected()) {
        strcpy(lastError, "NOT CONNECTED TO WIFI");
        return false;
    }

    String key = Config::wifi().wpaSecKey;
    if (key.isEmpty()) {
        strcpy(lastError, "NO WPA-SEC KEY CONFIGURED");
        return false;
    }

    strcpy(statusMessage, "FETCHING RESULTS...");
    Serial.println("[WPASEC] Fetching results from WPA-SEC");
    BusyScope busyGuard(busy);

    // Free caches and suspend sprites for maximum heap before TLS.
    freeCacheMemory();
    Display::requestSuspendSprites();
    Display::waitForSpritesSuspended(2000);
    ScopeResume resumeGuard;  // auto-resume on any exit

    // Guard: ensure enough contiguous heap for TLS (~16KB)
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 16000) {
        strcpy(lastError, "LOW HEAP");
        return false;
    }
    logHeap("fetch pre-TLS");

    // Retry once on transient TLS/connect failures (phone tether, weak RSSI)
    WiFiClientSecure client;
    client.setInsecure();
    client.setNoDelay(true);
    client.setTimeout(45000);

    bool connected = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (client.connect(API_HOST, 443)) {
            connected = true;
            break;
        }
        delay(250);
    }
    if (!connected) {
        strcpy(lastError, "TLS CONNECT FAIL");
        strcpy(statusMessage, "TLS FAIL");
        return false;
    }

    // Send manual GET
    client.print("GET ");
    client.print(RESULTS_PATH);
    client.println(" HTTP/1.1");
    client.print("Host: ");
    client.println(API_HOST);
    client.print("Cookie: key=");
    client.println(key);
    client.println("Connection: close");
    client.println();

    // Read status code (handle 100-Continue)
    int code = readHttpStatus(client);
    if (code != 200) {
        snprintf(lastError, sizeof(lastError), "HTTP %d", code);
        strcpy(statusMessage, "HTTP FAIL");
        client.stop();
        return false;
    }

    // Skip headers
    while (client.connected()) {
        String h = client.readStringUntil('\n');
        if (h == "\r" || h.length() == 0) break;
    }

    // Stream response to SD temp file to avoid heap spikes
    const char* TMP_FETCH_PATH = "/wpasec_fetch.tmp";
    if (SD.exists(TMP_FETCH_PATH)) SD.remove(TMP_FETCH_PATH);
    File tmp = SD.open(TMP_FETCH_PATH, FILE_WRITE);
    if (!tmp) {
        strcpy(lastError, "TMP FAIL");
        client.stop();
        return false;
    }

    char buf[256];
    uint32_t lastData = millis();
    while (client.connected() && (millis() - lastData) < 60000) {
        size_t n = client.readBytesUntil('\n', buf, sizeof(buf) - 1);
        if (n == 0) {
            if (client.available()) continue;
            delay(1);
            continue;
        }
        buf[n] = '\0';
        tmp.println(buf);
        lastData = millis();
    }
    tmp.close();
    client.stop();

    // Parse temp file line-by-line
    File parseFile = SD.open(TMP_FETCH_PATH, FILE_READ);
    if (!parseFile) {
        strcpy(lastError, "TMP READ FAIL");
        return false;
    }

    // Reload existing cache so we merge, not clobber
    loadCache();

    int newCracks = 0;
    while (parseFile.available()) {
        String line = parseFile.readStringUntil('\n');
        line.trim();
        if (line.isEmpty()) continue;

        int firstColon = line.indexOf(':');
        int lastColon  = line.lastIndexOf(':');

        if (firstColon > 0 && lastColon > firstColon) {
            String bssid = normalizeBSSID(line.substring(0, firstColon).c_str());
            String ssid  = line.substring(firstColon + 1, lastColon);
            String pass  = line.substring(lastColon + 1);

            int colonPos = ssid.indexOf(':');
            if (colonPos >= 0) {
                ssid = ssid.substring(colonPos + 1);
            }

            if (!pass.isEmpty() && bssid.length() >= 12) {
                bool isNew = (crackedCache.find(bssid) == crackedCache.end());
                if (isNew) newCracks++;

                CacheEntry ce;
                ce.ssid = ssid;
                ce.password = pass;

                crackedCache[bssid] = ce;
                uploadedCache[bssid] = true;
            }
        }
    }
    parseFile.close();
    SD.remove(TMP_FETCH_PATH);

    // Save caches
    saveCache();
    saveUploadedList();

    snprintf(statusMessage, sizeof(statusMessage), "%d cracked (%d new)",
             crackedCache.size(), newCracks);
    Serial.printf("[WPASEC] Fetched: %d total, %d new\n", crackedCache.size(), newCracks);
    logHeap("fetch end");
    SDLog::log("WPASEC", "Fetched: %d cracked (%d new)", (int)crackedCache.size(), newCracks);
    return true;
}


bool WPASec::uploadCapture(const char* pcapPath) {
    BusyScope busyGuard(busy);
    if (!isConnected()) {
        strcpy(lastError, "NOT CONNECTED TO WIFI");
        return false;
    }

    // Get API key
    String key = Config::wifi().wpaSecKey;
    if (key.isEmpty()) {
        strcpy(lastError, "NO WPA-SEC KEY CONFIGURED");
        return false;
    }

    // Check file exists
    if (!SD.exists(pcapPath)) {
        snprintf(lastError, sizeof(lastError), "FILE NOT FOUND: %s", pcapPath);
        return false;
    }

    // Open capture file
    File pcapFile = SD.open(pcapPath, FILE_READ);
    if (!pcapFile) {
        strcpy(lastError, "CANNOT OPEN FILE");
        return false;
    }

    const size_t fileSize = pcapFile.size();
    if (fileSize == 0) {
        pcapFile.close();
        strcpy(lastError, "EMPTY FILE");
        return false;
    }

    // Extract filename
    const char* slash = strrchr(pcapPath, '/');
    const char* fname = slash ? slash + 1 : pcapPath;
    String filename = fname;

    // Derive BSSID key from filename and prevent duplicate uploads.
    // (Messaging parity: always show "ALREADY UPLOADED" and hard-stop.)
    String base = filename;
    int dot = base.indexOf('.');
    if (dot > 0) base = base.substring(0, dot);
    if (base.endsWith("_hs")) base = base.substring(0, base.length() - 3);
    String normKey = normalizeBSSID(base.c_str());

    if (fileContainsKey(UPLOADED_FILE, normKey) || fileContainsKey(CACHE_FILE, normKey)) {
        pcapFile.close();
        strcpy(statusMessage, "ALREADY UPLOADED");
        strcpy(lastError, "ALREADY UPLOADED");
        SDLog::log("WPASEC", "Skip re-upload: %s", normKey.c_str());
        return false;
    }

    Serial.printf("[WPASEC] Uploading %s (%u bytes) to %s%s\n",
                  filename.c_str(), (unsigned int)fileSize, API_HOST, SUBMIT_PATH);

    // Free caches and suspend sprites for maximum heap before TLS
    freeCacheMemory();
    Display::requestSuspendSprites();
    Display::waitForSpritesSuspended(2000);
    ScopeResume resumeGuard;  // auto-resume on any exit

    // Guard: ensure enough contiguous heap for TLS (~16KB)
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 16000) {
        strcpy(lastError, "LOW HEAP");
        return false;
    }

    // Setup TLS client (longer timeout improves reliability on slow links)
    WiFiClientSecure client;
    client.setInsecure();
    client.setNoDelay(true);
    client.setTimeout(60000);

    String url = String("https://") + API_HOST + SUBMIT_PATH;

    String boundary = "----WPASEC";
    boundary += String((unsigned long)millis());
    String bodyStart = "--" + boundary + "\r\n";
    bodyStart += "Content-Disposition: form-data; name=\"file\"; filename=\"" + filename + "\"\r\n";
    bodyStart += "Content-Type: application/octet-stream\r\n\r\n";
    String bodyEnd = "\r\n--" + boundary + "--\r\n";
    size_t contentLength = bodyStart.length() + fileSize + bodyEnd.length();

    // Build request
    String cookie = String("key=") + key;

    // Send request manually over WiFiClientSecure
    bool success = false;
    int lastCode = -1;

    for (int attempt = 0; attempt < 2 && !success; ++attempt) {
        // Reset file cursor for retry
        pcapFile.seek(0);

        if (!client.connect(API_HOST, 443)) {
            lastCode = -1;
            if (attempt == 0) {
                delay(250);
                continue;
            }
            pcapFile.close();
            strcpy(lastError, "TLS CONNECT FAIL");
            strcpy(statusMessage, "TLS FAIL");
            return false;
        }

        client.print("POST ");
        client.print(SUBMIT_PATH);
        client.println(" HTTP/1.1");
        client.print("Host: ");
        client.println(API_HOST);
        client.print("Cookie: ");
        client.println(cookie);
        client.print("Content-Type: multipart/form-data; boundary=");
        client.println(boundary);
        client.print("Content-Length: ");
        client.println((unsigned int)contentLength);
        client.println("Connection: close");
        client.println();

        client.print(bodyStart);

        // Stream file
        uint8_t buf[1024];
        size_t remaining = fileSize;
        while (remaining > 0) {
            size_t toRead = remaining > sizeof(buf) ? sizeof(buf) : remaining;
            size_t n = pcapFile.read(buf, toRead);
            if (n == 0) {
                strcpy(lastError, "READ ERROR");
                client.stop();
                pcapFile.close();
                return false;
            }
            size_t written = client.write(buf, n);
            if (written != n) {
                strcpy(lastError, "WRITE ERROR");
                client.stop();
                pcapFile.close();
                return false;
            }
            remaining -= n;
        }

        client.print(bodyEnd);
        client.flush();

        // Read response status
        lastCode = readHttpStatus(client);
        client.stop();

        if (lastCode == 200 || lastCode == 302) {
            success = true;
            break;
        }

        // Retry once on transient server-side throttling/timeout
        if (attempt == 0 && (lastCode == 408 || lastCode == 429 || lastCode >= 500)) {
            delay(400);
            continue;
        }
    }

    // Close capture file after attempts
    pcapFile.close();

    if (success) {
        strcpy(statusMessage, "UPLOAD OK");
        markUploaded(normKey.c_str());

        Serial.printf("[WPASEC] Upload successful, marked %s as uploaded\n", base.c_str());

        if (shouldAwardSmokedBacon()) {
            XP::addXP(XPEvent::SMOKED_BACON);
            char toast[32];
            snprintf(toast, sizeof(toast), "SMOKED BACON\n+%u XP",
                     (unsigned)XP::getLastXPGainAmount());
            Display::requestTopBarMessage(toast, 2500);
        }
        logHeap("upload success");
        SDLog::log("WPASEC", "Upload OK: %s", filename.c_str());
        return true;
    }

    snprintf(lastError, sizeof(lastError), "UPLOAD HTTP %d", lastCode);
    strcpy(statusMessage, "UPLOAD FAILED");
    Serial.printf("[WPASEC] Upload failed: HTTP %d\n", lastCode);
    logHeap("upload fail");
    SDLog::log("WPASEC", "Upload failed: %s (HTTP %d)", filename.c_str(), lastCode);
    return false;
}

const char* WPASec::getLastError() {
    return lastError;
}

const char* WPASec::getStatus() {
    return statusMessage;
}
