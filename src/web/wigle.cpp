// WiGLE wardriving service client implementation

#include "wigle.h"
#include <WiFi.h>
#include "../core/wifi_utils.h"
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <SD.h>
#include <base64.h>
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../ui/display.h"
#include "../core/xp.h"
#include "../core/scope_resume.h"

// Static member initialization
char WiGLE::lastError[64] = "";
char WiGLE::statusMessage[64] = "READY";
std::vector<String> WiGLE::uploadedFiles;
bool WiGLE::listLoaded = false;
bool WiGLE::busy = false;

class WigleBusyScope {
public:
    explicit WigleBusyScope(bool& ref) : ref_(ref) { ref_ = true; }
    ~WigleBusyScope() { ref_ = false; }
private:
    bool& ref_;
};

// Read and parse HTTP status code from a client response.
// Handles optional "HTTP/1.1 100 Continue" prelude.
static int readHttpStatus(WiFiClientSecure& client) {
    for (int guard = 0; guard < 6; ++guard) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (!line.startsWith("HTTP/")) {
            continue;
        }
        int sp1 = line.indexOf(' ');
        if (sp1 < 0) return -1;
        int sp2 = line.indexOf(' ', sp1 + 1);
        String codeStr = (sp2 > sp1) ? line.substring(sp1 + 1, sp2) : line.substring(sp1 + 1);
        int code = codeStr.toInt();

        if (code == 100) {
            // Consume headers for the 100 response
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

static bool shouldAwardSmokedBacon() {
    uint8_t chance = 3;  // base 3%
    if (XP::getClass() == PorkClass::B4C0NM4NC3R) {
        chance += 1;  // +1% for Baconmancer
    }
    return (esp_random() % 100) < chance;
}

void WiGLE::init() {
    uploadedFiles.clear();
    listLoaded = false;
    strcpy(lastError, "");
    strcpy(statusMessage, "READY");
    loadUploadedList();
}

// ============================================================================
// WiFi Connection (Standalone)
// ============================================================================

bool WiGLE::connect() {
    WigleBusyScope busyGuard(busy);
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
    Serial.printf("[WIGLE] Connecting to %s\n", ssid.c_str());
    Serial.printf("[WIGLE] WiFi pre-reset: mode=%d status=%d\n", WiFi.getMode(), WiFi.status());

    // Ensure promiscuous mode is off before starting web operations
    // This is important if coming from modes like Oink that use promiscuous mode
    WiFiUtils::stopPromiscuous();


    // Suspend display sprites to free contiguous heap for WiFi/TLS operations
    Display::requestSuspendSprites();
    Display::waitForSpritesSuspended(2000);

    // Free uploaded list to save additional memory before connect
    freeUploadedListMemory();

    // Small delay to allow memory management to complete
    delay(50);
    yield();

    WiFiUtils::hardReset();
    Serial.printf("[WIGLE] WiFi post-reset: mode=%d status=%d\n", WiFi.getMode(), WiFi.status());

    WiFi.begin(ssid.c_str(), password.c_str());
    
    // Wait for connection with timeout
    uint32_t startTime = millis();
    uint32_t lastLog = startTime;
    // Slightly longer timeout helps when phone tethering or on weak RSSI.
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 20000) {
        if (millis() - lastLog >= 1000) {
            lastLog = millis();
            Serial.printf("[WIGLE] Connecting... status=%d\n", WiFi.status());
        }
        delay(100);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        snprintf(statusMessage, sizeof(statusMessage), "IP: %s", WiFi.localIP().toString().c_str());
        Serial.printf("[WIGLE] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        Display::requestResumeSprites();
        return true;
    }
    
    strcpy(lastError, "CONNECTION TIMEOUT");
    strcpy(statusMessage, "CONNECT FAILED");
    Serial.printf("[WIGLE] Connection failed (mode=%d status=%d)\n", WiFi.getMode(), WiFi.status());
    WiFiUtils::shutdown();
    Display::requestResumeSprites();
    return false;
}

void WiGLE::disconnect() {
    Serial.printf("[WIGLE] Disconnect (mode=%d status=%d)\n", WiFi.getMode(), WiFi.status());
    WiFiUtils::shutdown();
    strcpy(statusMessage, "DISCONNECTED");
    Serial.println("[WIGLE] Disconnected");
}

bool WiGLE::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

bool WiGLE::isBusy() {
    return busy;
}

// ============================================================================
// Upload Tracking
// ============================================================================

bool WiGLE::loadUploadedList() {
    if (listLoaded) return true;  // Already loaded, skip SD read
    
    uploadedFiles.clear();
    uploadedFiles.reserve(200);  // Cap enforced; reserve to avoid fragmentation
    
    if (!SD.exists(UPLOADED_FILE)) {
        listLoaded = true;
        return true;  // No file yet, that's OK
    }
    
    File f = SD.open(UPLOADED_FILE, FILE_READ);
    if (!f) return false;
    
    while (f.available() && uploadedFiles.size() < 200) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (!line.isEmpty() && line.length() < 100) {  // Sanity check
            uploadedFiles.push_back(line);
        }
    }
    
    f.close();
    listLoaded = true;
    Serial.printf("[WIGLE] Loaded %d uploaded files from tracking\n", uploadedFiles.size());
    return true;
}

bool WiGLE::saveUploadedList() {
    File f = SD.open(UPLOADED_FILE, FILE_WRITE);
    if (!f) return false;
    
    for (const auto& filename : uploadedFiles) {
        f.println(filename);
    }
    
    f.close();
    return true;
}

/**
 * Free the uploaded files list from memory.
 *
 * This helper clears the in‑memory vector used to track which files have
 * already been uploaded.  It also marks the list as not loaded so that
 * a subsequent call to loadUploadedList() will reload the data from
 * storage.  Callers should ensure any pending changes have been
 * persisted via saveUploadedList() prior to invoking this function.
 */
void WiGLE::freeUploadedListMemory() {
    size_t count = uploadedFiles.size();
    uploadedFiles.clear();
    listLoaded = false;
    Serial.printf("[WIGLE] Freed uploaded list (%u entries). Heap free=%lu largest=%lu\n",
                  (unsigned int)count,
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool WiGLE::isUploaded(const char* filename) {
    loadUploadedList();
    String name = getFilenameFromPath(filename);
    for (const auto& uploaded : uploadedFiles) {
        if (uploaded == name) return true;
    }
    return false;
}

void WiGLE::markUploaded(const char* filename) {
    // Ensure the list is loaded
    loadUploadedList();
    String name = getFilenameFromPath(filename);
    // If already present, do nothing
    for (const auto& uploaded : uploadedFiles) {
        if (uploaded == name) return;
    }
    // Cap the list at 200 entries (remove oldest if necessary)
    if (uploadedFiles.size() >= 200) {
        uploadedFiles.erase(uploadedFiles.begin());
    }
    uploadedFiles.push_back(name);
    // Persist the updated list
    saveUploadedList();
}

void WiGLE::removeFromUploaded(const char* filename) {
    String name = getFilenameFromPath(filename);
    for (auto it = uploadedFiles.begin(); it != uploadedFiles.end(); ++it) {
        if (*it == name) {
            uploadedFiles.erase(it);
            saveUploadedList();
            Serial.printf("[WIGLE] Removed from uploaded tracking: %s\n", name.c_str());
            return;
        }
    }
}

uint16_t WiGLE::getUploadedCount() {
    loadUploadedList();
    return uploadedFiles.size();
}

String WiGLE::getFilenameFromPath(const char* path) {
    String fullPath = path;
    int lastSlash = fullPath.lastIndexOf('/');
    if (lastSlash >= 0) {
        return fullPath.substring(lastSlash + 1);
    }
    return fullPath;
}

bool WiGLE::hasCredentials() {
    return !Config::wifi().wigleApiName.isEmpty() && 
           !Config::wifi().wigleApiToken.isEmpty();
}

// ============================================================================
// API Operations
// ============================================================================

bool WiGLE::uploadFile(const char* csvPath) {
    WigleBusyScope busyGuard(busy);
    // Suspend sprites and free lists to maximize heap for TLS
    Display::requestSuspendSprites();
    Display::waitForSpritesSuspended(2000);
    freeUploadedListMemory();
    ScopeResume resumeGuard;  // auto-resume on all exits

    // Guard: ensure enough contiguous heap for TLS (~12KB target)
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 12000) {
        strcpy(lastError, "LOW HEAP");
        return false;
    }
    Serial.printf("[WIGLE][HEAP] upload start free=%lu largest=%lu\n",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    if (!isConnected()) {
        strcpy(lastError, "NOT CONNECTED TO WIFI");
        return false;
    }

    if (!hasCredentials()) {
        strcpy(lastError, "NO WIGLE API CREDENTIALS");
        Display::requestResumeSprites();
        return false;
    }

    if (!SD.exists(csvPath)) {
        snprintf(lastError, sizeof(lastError), "FILE NOT FOUND");
        Display::requestResumeSprites();
        return false;
    }

    File csvFile = SD.open(csvPath, FILE_READ);
    if (!csvFile) {
        strcpy(lastError, "CANNOT OPEN FILE");
        return false;
    }

    size_t fileSize = csvFile.size();
    // WiGLE limit is 180MB, but we'll be more conservative on ESP32
    if (fileSize > 500000) {  // 500KB limit for ESP32 memory safety
        strcpy(lastError, "FILE TOO LARGE (>500KB)");
        csvFile.close();
        return false;
    }

    strcpy(statusMessage, "UPLOADING...");
    Serial.printf("[WIGLE] Uploading %s (%d bytes)\n", csvPath, fileSize);

    // Build Basic Auth header
    String apiName = Config::wifi().wigleApiName;
    String apiToken = Config::wifi().wigleApiToken;
    String credentials = apiName + ":" + apiToken;
    String authHeader = "Basic " + base64::encode(credentials);

    // Prepare multipart form parts
    char boundary[48];
    snprintf(boundary, sizeof(boundary), "----PorkchopWiGLE%lu", (unsigned long)millis());

    char filenameBuf[96];
    {
        String name = getFilenameFromPath(csvPath);
        strncpy(filenameBuf, name.c_str(), sizeof(filenameBuf) - 1);
        filenameBuf[sizeof(filenameBuf) - 1] = '\0';
    }

    String bodyStart = "--" + String(boundary) + "\r\n";
    bodyStart += "Content-Disposition: form-data; name=\"file\"; filename=\"";
    bodyStart += filenameBuf;
    bodyStart += "\"\r\n";
    bodyStart += "Content-Type: text/csv\r\n\r\n";
    String bodyEnd = "\r\n--" + String(boundary) + "--\r\n";
    size_t contentLength = bodyStart.length() + fileSize + bodyEnd.length();

    // Use bare WiFiClientSecure to avoid HTTPClient heap reuse
    WiFiClientSecure client;
    client.setInsecure();
    client.setNoDelay(true);
    client.setTimeout(60000);
    Serial.printf("[WIGLE][HEAP] before HTTP begin free=%lu largest=%lu\n",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

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
        csvFile.close();
        return false;
    }

    // Send request manually to stream file without buffering whole body
    client.printf("POST %s HTTP/1.1\r\n", UPLOAD_PATH);
    client.printf("Host: %s\r\n", API_HOST);
    client.print("Authorization: ");
    client.print(authHeader);
    client.print("\r\n");
    client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
    client.printf("Content-Length: %u\r\n", (unsigned int)contentLength);
    client.print("Connection: close\r\n\r\n");

    client.print(bodyStart);

    const size_t CHUNK_SIZE = 1024;
    uint8_t chunk[CHUNK_SIZE];
    size_t bytesRemaining = fileSize;
    while (bytesRemaining > 0) {
        size_t toRead = (bytesRemaining > CHUNK_SIZE) ? CHUNK_SIZE : bytesRemaining;
        size_t bytesRead = csvFile.read(chunk, toRead);
        if (bytesRead == 0) {
            strcpy(lastError, "READ ERROR");
            csvFile.close();
            client.stop();
            return false;
        }
        size_t written = client.write(chunk, bytesRead);
        if (written != bytesRead) {
            strcpy(lastError, "WRITE ERROR");
            csvFile.close();
            client.stop();
            return false;
        }
        bytesRemaining -= bytesRead;
    }
    csvFile.close();

    client.print(bodyEnd);
    client.flush();

    // Read response status code (handles optional 100-Continue)
    int statusCode = readHttpStatus(client);

    // Skip headers to reach body
    while (client.connected()) {
        String h = client.readStringUntil('\n');
        if (h == "\r" || h.length() == 0) break;
    }

    // Read a small body for error context
    String body;
    uint32_t startBody = millis();
    while ((client.connected() || client.available()) &&
           body.length() < 256 &&
           (millis() - startBody) < 5000) {
        if (client.available()) {
            body += (char)client.read();
        } else {
            delay(1);
        }
    }

    // Close connection
    client.stop();
    Display::requestResumeSprites();

    if (statusCode == 200 || statusCode == 302) {
        strcpy(statusMessage, "UPLOAD OK");
        markUploaded(csvPath);
        if (shouldAwardSmokedBacon()) {
            XP::addXP(XPEvent::SMOKED_BACON);
            char toast[32];
            snprintf(toast, sizeof(toast), "SMOKED BACON\n+%u XP",
                     (unsigned)XP::getLastXPGainAmount());
            Display::requestTopBarMessage(toast, 2500);
        }
        SDLog::log("WIGLE", "Upload OK: %s", filenameBuf);
        Serial.printf("[WIGLE][HEAP] upload success free=%lu largest=%lu\n",
                      (unsigned long)ESP.getFreeHeap(),
                      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return true;
    }

    // Build a clearer error message
    char statusTxt[16];
    if (statusCode > 0) {
        snprintf(statusTxt, sizeof(statusTxt), "HTTP %d", statusCode);
    } else {
        strncpy(statusTxt, "NO RESPONSE", sizeof(statusTxt) - 1);
        statusTxt[sizeof(statusTxt) - 1] = '\0';
    }

    if (body.length() > 0) {
        snprintf(lastError, sizeof(lastError), "UPLOAD FAILED: %s | %s", statusTxt, body.c_str());
    } else {
        snprintf(lastError, sizeof(lastError), "UPLOAD FAILED: %s", statusTxt);
    }
    strcpy(statusMessage, "UPLOAD FAILED");
    SDLog::log("WIGLE", "Upload failed: %s", filenameBuf);
    Serial.printf("[WIGLE][HEAP] upload fail free=%lu largest=%lu\n",
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return false;
}

const char* WiGLE::getLastError() {
    return lastError;
}

const char* WiGLE::getStatus() {
    return statusMessage;
}

