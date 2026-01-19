// Captures Menu - View saved handshake captures

#include "captures_menu.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include "display.h"
#include "../web/wpasec.h"
#include "../core/config.h"

// For heap statistics logging
#include <esp_heap_caps.h>

// Static member initialization
std::vector<CaptureInfo> CapturesMenu::captures;
uint8_t CapturesMenu::selectedIndex = 0;
uint8_t CapturesMenu::scrollOffset = 0;
bool CapturesMenu::active = false;
bool CapturesMenu::keyWasPressed = false;
bool CapturesMenu::nukeConfirmActive = false;
bool CapturesMenu::detailViewActive = false;
bool CapturesMenu::connectingWiFi = false;
bool CapturesMenu::uploadingFile = false;
bool CapturesMenu::refreshingResults = false;

TaskHandle_t CapturesMenu::wpaTaskHandle = nullptr;
volatile bool CapturesMenu::wpaTaskDone = false;
volatile bool CapturesMenu::wpaTaskSuccess = false;
volatile CapturesMenu::WpaTaskAction CapturesMenu::wpaTaskAction = CapturesMenu::WpaTaskAction::NONE;
uint8_t CapturesMenu::wpaTaskIndex = 0;
char CapturesMenu::wpaTaskResultMsg[64] = {0};

void CapturesMenu::wpaTaskFn(void* pv) {
    WpaTaskCtx* ctx = reinterpret_cast<WpaTaskCtx*>(pv);

    // Run WiFi + TLS from a dedicated task with a large stack.
    // This avoids stack canary panics from mbedTLS handshake inside loopTask.

    bool weConnected = false;
    if (!WPASec::isConnected()) {
        connectingWiFi = true;
        if (!WPASec::connect()) {
            strncpy(wpaTaskResultMsg, WPASec::getLastError(), sizeof(wpaTaskResultMsg) - 1);
            wpaTaskResultMsg[sizeof(wpaTaskResultMsg) - 1] = '\0';
            wpaTaskSuccess = false;
            wpaTaskAction = ctx->action;
            wpaTaskIndex = ctx->index;
            wpaTaskDone = true;
            connectingWiFi = false;
            delete ctx;
            vTaskDelete(nullptr);
            return;
        }
        weConnected = true;
        connectingWiFi = false;
    }

    bool ok = false;
    if (ctx->action == WpaTaskAction::UPLOAD) {
        uploadingFile = true;
        ok = WPASec::uploadCapture(ctx->pcapPath);
        uploadingFile = false;
    } else if (ctx->action == WpaTaskAction::REFRESH) {
        refreshingResults = true;
        ok = WPASec::fetchResults();
        refreshingResults = false;
    }

    if (weConnected) {
        WPASec::disconnect();
    }

    wpaTaskSuccess = ok;
    wpaTaskAction = ctx->action;
    wpaTaskIndex = ctx->index;
    if (ok) {
        if (ctx->action == WpaTaskAction::UPLOAD) {
            strncpy(wpaTaskResultMsg, "UPLOAD OK!", sizeof(wpaTaskResultMsg) - 1);
        } else {
            // fetchResults has a user-friendly status string
            strncpy(wpaTaskResultMsg, WPASec::getStatus(), sizeof(wpaTaskResultMsg) - 1);
        }
    } else {
        strncpy(wpaTaskResultMsg, WPASec::getLastError(), sizeof(wpaTaskResultMsg) - 1);
    }
    wpaTaskResultMsg[sizeof(wpaTaskResultMsg) - 1] = '\0';

    wpaTaskDone = true;

    delete ctx;
    vTaskDelete(nullptr);
}

void CapturesMenu::init() {
    captures.clear();
    selectedIndex = 0;
    scrollOffset = 0;
}

void CapturesMenu::show() {
    active = true;
    selectedIndex = 0;
    scrollOffset = 0;
    keyWasPressed = true;  // Ignore the Enter that selected us from menu

    // If scan fails, the captures list will remain empty
    // This is handled by the draw function which shows "No captures found"
    scanCaptures();
}

void CapturesMenu::hide() {
    active = false;
    captures.clear();
    captures.shrink_to_fit();  // Release vector memory
}

bool CapturesMenu::scanCaptures() {
    captures.clear();

    // Guard: Skip if no SD card available
    if (!Config::isSDAvailable()) {
        Serial.println("[CAPTURES] No SD card available");
        return false;
    }

    // Create directory if it doesn't exist
    if (!SD.exists("/handshakes")) {
        Serial.println("[CAPTURES] No handshakes directory, creating...");
        if (!SD.mkdir("/handshakes")) {
            Serial.println("[CAPTURES] Failed to create handshakes directory");
            return false;
        }
    }

    File dir = SD.open("/handshakes");
    if (!dir || !dir.isDirectory()) {
        Serial.println("[CAPTURES] Failed to open handshakes directory");
        return false;
    }

    File file = dir.openNextFile();
    while (file) {
        if (captures.size() >= MAX_CAPTURES) {
            Serial.printf("[CAPTURES] Cap reached (%u), skipping rest\n", (unsigned)MAX_CAPTURES);
            file.close();
            break;
        }
        String name = file.name();
        bool isPCAP = name.endsWith(".pcap");
        bool isPMKID = name.endsWith(".22000") && !name.endsWith("_hs.22000");
        bool isHS22000 = name.endsWith("_hs.22000");

        // Skip PCAP if we have the corresponding _hs.22000 (avoid duplicates).
        // Prefer showing _hs.22000 because it's hashcat-ready.
        if (isPCAP) {
            String baseName = name.substring(0, name.indexOf('.'));
            String hs22kPath = "/handshakes/" + baseName + "_hs.22000";
            if (SD.exists(hs22kPath)) {
                // Close current file before skipping to next to avoid leaking file handles
                file.close();
                file = dir.openNextFile();
                continue;
            }
        }

        if (isPCAP || isPMKID || isHS22000) {
            CaptureInfo info;
            info.filename = name;
            info.fileSize = file.size();
            info.captureTime = file.getLastWrite();
            info.isPMKID = isPMKID;  // Only true for PMKID files

            // Extract BSSID from filename (e.g., "64EEB7208286.pcap" or "64EEB7208286_hs.22000")
            String baseName = name.substring(0, name.indexOf('.'));
            // Handle _hs suffix for handshake 22000 files
            if (baseName.endsWith("_hs")) {
                baseName = baseName.substring(0, baseName.length() - 3);
            }
            if (baseName.length() >= 12) {
                info.bssid = baseName.substring(0, 2) + ":" +
                             baseName.substring(2, 4) + ":" +
                             baseName.substring(4, 6) + ":" +
                             baseName.substring(6, 8) + ":" +
                             baseName.substring(8, 10) + ":" +
                             baseName.substring(10, 12);
            } else {
                info.bssid = baseName;
            }

            // Try to get SSID from companion .txt file if it exists. For PMKID we use _pmkid.txt suffix, otherwise .txt
            String txtPath = isPMKID ?
                "/handshakes/" + baseName + "_pmkid.txt" :
                "/handshakes/" + baseName + ".txt";
            if (SD.exists(txtPath)) {
                File txtFile = SD.open(txtPath, FILE_READ);
                if (txtFile) {
                    info.ssid = txtFile.readStringUntil('\n');
                    info.ssid.trim();
                    txtFile.close();
                }
            }
            if (info.ssid.isEmpty()) {
                info.ssid = "[UNKNOWN]";
            }

            // Default status and password
            info.status = CaptureStatus::LOCAL;
            info.password = "";

            captures.push_back(info);
        }

        // Close current file and move to next
        file.close();
        file = dir.openNextFile();
    }
    dir.close();

    // Update WPA-SEC status for all captures
    updateWPASecStatus();

    // Sort by capture time (newest first)
    std::sort(captures.begin(), captures.end(), [](const CaptureInfo& a, const CaptureInfo& b) {
        return a.captureTime > b.captureTime;
    });

    Serial.printf("[CAPTURES] Found %d captures\n", captures.size());
    return true;
}

void CapturesMenu::updateWPASecStatus() {
    // Load WPA-SEC cache (lazy, only loads once)
    WPASec::loadCache();
    
    for (auto& cap : captures) {
        // Normalize BSSID for lookup (remove colons)
        String normalBssid = cap.bssid;
        normalBssid.replace(":", "");
        
        if (WPASec::isCracked(normalBssid.c_str())) {
            cap.status = CaptureStatus::CRACKED;
            cap.password = WPASec::getPassword(normalBssid.c_str());
        } else if (WPASec::isUploaded(normalBssid.c_str())) {
            cap.status = CaptureStatus::UPLOADED;
        } else {
            cap.status = CaptureStatus::LOCAL;
        }
    }
}

void CapturesMenu::update() {
    if (!active) return;

    // Handle completion of background WPA‑SEC task
    if (wpaTaskHandle != nullptr && wpaTaskDone) {
        // Task self-deletes; clear handle and report result in the UI thread.
        wpaTaskHandle = nullptr;
        connectingWiFi = false;
        uploadingFile = false;
        refreshingResults = false;

        // Surface result in the top bar
        const char* msg = wpaTaskResultMsg[0] ? wpaTaskResultMsg : (wpaTaskSuccess ? "OK" : "FAIL");
        Display::setTopBarMessage(String("WPA-SEC ") + msg, 4000);

        // Update capture statuses after task completion
        updateWPASecStatus();
        WPASec::freeCacheMemory();

        wpaTaskDone = false;
    }

    handleInput();
}

void CapturesMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();
    
    // Handle nuke confirmation modal
    if (nukeConfirmActive) {
        if (M5Cardputer.Keyboard.isKeyPressed('y') || M5Cardputer.Keyboard.isKeyPressed('Y')) {
            nukeLoot();
            nukeConfirmActive = false;
            Display::clearBottomOverlay();
            scanCaptures();  // Refresh list (should be empty now)
        } else if (M5Cardputer.Keyboard.isKeyPressed('n') || M5Cardputer.Keyboard.isKeyPressed('N') ||
                   M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE) || keys.enter) {
            nukeConfirmActive = false;  // Cancel
            Display::clearBottomOverlay();
        }
        return;
    }
    
    // Handle detail view modal - Enter/backspace closes, U/R trigger actions
    if (detailViewActive) {
        if (keys.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            detailViewActive = false;
            return;
        }
        // Allow U/R in modal - close modal and trigger action
        if (M5Cardputer.Keyboard.isKeyPressed('u') || M5Cardputer.Keyboard.isKeyPressed('U')) {
            detailViewActive = false;
            if (!captures.empty() && selectedIndex < captures.size()) {
                uploadSelected();
            }
            return;
        }
        if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R')) {
            detailViewActive = false;
            refreshResults();
            return;
        }
        return;  // Block other inputs while detail view is open
    }
    
    // Navigation with ; (up) and . (down)
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (selectedIndex > 0) {
            selectedIndex--;
            if (selectedIndex < scrollOffset) {
                scrollOffset = selectedIndex;
            }
        }
    }
    
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (!captures.empty() && selectedIndex < captures.size() - 1) {
            selectedIndex++;
            if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
            }
        }
    }
    
    // Enter shows detail view (password if cracked)
    if (keys.enter) {
        if (!captures.empty() && selectedIndex < captures.size()) {
            detailViewActive = true;
        }
    }
    
    // Nuke all loot with D key
    if (M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D')) {
        if (!captures.empty()) {
            nukeConfirmActive = true;
            Display::setBottomOverlay("PERMANENT | NO UNDO");
        }
    }
    
    // U key uploads selected capture to WPA-SEC
    if (M5Cardputer.Keyboard.isKeyPressed('u') || M5Cardputer.Keyboard.isKeyPressed('U')) {
        if (!captures.empty() && selectedIndex < captures.size()) {
            uploadSelected();
        }
    }
    
    // R key refreshes results from WPA-SEC
    if (M5Cardputer.Keyboard.isKeyPressed('r') || M5Cardputer.Keyboard.isKeyPressed('R')) {
        refreshResults();
    }
    
    // Backspace - go back
    if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        hide();
    }
}

void CapturesMenu::formatTime(char* out, size_t len, time_t t) {
    if (!out || len == 0) return;
    if (t == 0) {
        strncpy(out, "UNKNOWN", len - 1);
        out[len - 1] = '\0';
        return;
    }
    
    struct tm* timeinfo = localtime(&t);
    if (!timeinfo) {
        strncpy(out, "UNKNOWN", len - 1);
        out[len - 1] = '\0';
        return;
    }
    
    // Format: "Dec 06 14:32"
    strftime(out, len, "%b %d %H:%M", timeinfo);
}

void CapturesMenu::draw(M5Canvas& canvas) {
    if (!active) return;

    canvas.fillSprite(COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);

    // Check if SD card is not available
    if (!Config::isSDAvailable()) {
        canvas.setCursor(4, 40);
        canvas.print("NO SD CARD!");
        canvas.setCursor(4, 55);
        canvas.print("INSERT AND RESTART");
        return;
    }

    if (captures.empty()) {
        canvas.setCursor(4, 40);
        canvas.print("No captures found");
        canvas.setCursor(4, 55);
        canvas.print("[O] to hunt.");
        return;
    }

    // Draw captures list
    int y = 2;
    int lineHeight = 18;

    for (uint8_t i = scrollOffset; i < captures.size() && i < scrollOffset + VISIBLE_ITEMS; i++) {
        const CaptureInfo& cap = captures[i];

        // Highlight selected
        if (i == selectedIndex) {
            canvas.fillRect(0, y - 1, canvas.width(), lineHeight, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }

        // SSID (truncated if needed) - show [P] prefix for PMKID, status indicator
        canvas.setCursor(4, y);
        char ssidBuf[24];
        size_t pos = 0;
        if (cap.isPMKID && sizeof(ssidBuf) > 4) {
            ssidBuf[pos++] = '[';
            ssidBuf[pos++] = 'P';
            ssidBuf[pos++] = ']';
        }
        const char* ssidSrc = cap.ssid.c_str();
        while (*ssidSrc && pos + 1 < sizeof(ssidBuf)) {
            ssidBuf[pos++] = (char)toupper((unsigned char)*ssidSrc++);
        }
        ssidBuf[pos] = '\0';
        if (pos > 16 && sizeof(ssidBuf) > 16) {
            ssidBuf[14] = '.';
            ssidBuf[15] = '.';
            ssidBuf[16] = '\0';
        }
        canvas.print(ssidBuf);

        // Status indicator
        canvas.setCursor(105, y);
        if (cap.status == CaptureStatus::CRACKED) {
            canvas.print("[OK]");
        } else if (cap.status == CaptureStatus::UPLOADED) {
            canvas.print("[..]");
        } else {
            canvas.print("[--]");
        }

        // Date/time
        canvas.setCursor(135, y);
        char timeBuf[20];
        formatTime(timeBuf, sizeof(timeBuf), cap.captureTime);
        canvas.print(timeBuf);

        // File size (KB)
        canvas.setCursor(210, y);
        canvas.printf("%dK", cap.fileSize / 1024);

        y += lineHeight;
    }

    // Scroll indicators
    if (scrollOffset > 0) {
        canvas.setCursor(canvas.width() - 10, 16);
        canvas.setTextColor(COLOR_FG);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < captures.size()) {
        canvas.setCursor(canvas.width() - 10, 16 + (VISIBLE_ITEMS - 1) * lineHeight);
        canvas.setTextColor(COLOR_FG);
        canvas.print("v");
    }

    // Draw nuke confirmation modal if active
    if (nukeConfirmActive) {
        drawNukeConfirm(canvas);
    }

    // Draw detail view modal if active
    if (detailViewActive) {
        drawDetailView(canvas);
    }

    // BSSID shown in bottom bar via getSelectedBSSID()
}

void CapturesMenu::drawNukeConfirm(M5Canvas& canvas) {
    // Modal box dimensions - matches PIGGYBLUES warning style
    const int boxW = 200;
    const int boxH = 70;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;
    
    // Black border then pink fill
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(1);
    
    int centerX = canvas.width() / 2;
    
    // Hacker edgy message
    canvas.drawString("!! SCORCHED EARTH !!", centerX, boxY + 8);
    canvas.drawString("rm -rf /handshakes/*", centerX, boxY + 22);
    canvas.drawString("THIS KILLS THE LOOT.", centerX, boxY + 36);
    canvas.drawString("[Y] DO IT  [N] ABORT", centerX, boxY + 54);
}

void CapturesMenu::nukeLoot() {
    Serial.println("[CAPTURES] Nuking all loot...");
    
    if (!SD.exists("/handshakes")) {
        return;
    }
    
    File dir = SD.open("/handshakes");
    if (!dir || !dir.isDirectory()) {
        return;
    }
    
    // Collect filenames first (can't delete while iterating)
    std::vector<String> files;
    File file = dir.openNextFile();
    while (file) {
        files.push_back(String("/handshakes/") + file.name());
        // Always close file handle to avoid exhausting SD file descriptors
        file.close();
        file = dir.openNextFile();
    }
    dir.close();
    
    // Delete all files
    int deleted = 0;
    for (const auto& path : files) {
        if (SD.remove(path)) {
            deleted++;
        }
    }
    
    Serial.printf("[CAPTURES] Nuked %d files\n", deleted);
    
    // Reset selection
    selectedIndex = 0;
    scrollOffset = 0;
    captures.clear();
}

const char* CapturesMenu::getSelectedBSSID() {
    if (selectedIndex < captures.size()) {
        const CaptureInfo& cap = captures[selectedIndex];
        // PMKIDs can't be uploaded to WPA-SEC (requires PCAP)
        if (cap.isPMKID) {
            return "L0C4L CR4CK: [R] [D]";
        }
        return "CR4CK TH3 L00T: [U] [R] [D]";
    }
    return "CR4CK TH3 L00T: [U] [R] [D]";
}
void CapturesMenu::drawDetailView(M5Canvas& canvas) {
    if (selectedIndex >= captures.size()) return;
    
    const CaptureInfo& cap = captures[selectedIndex];
    
    // Modal box dimensions
    const int boxW = 220;
    const int boxH = 85;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2 - 5;
    
    // Black border then pink fill
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 8, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 8, COLOR_FG);
    
    // Black text on pink background
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    canvas.setTextSize(1);
    
    int centerX = canvas.width() / 2;
    
    // SSID
    char ssidLine[24];
    size_t ssidPos = 0;
    const char* ssidSrc = cap.ssid.c_str();
    while (*ssidSrc && ssidPos + 1 < sizeof(ssidLine)) {
        ssidLine[ssidPos++] = (char)toupper((unsigned char)*ssidSrc++);
    }
    ssidLine[ssidPos] = '\0';
    if (ssidPos > 16 && sizeof(ssidLine) > 16) {
        ssidLine[14] = '.';
        ssidLine[15] = '.';
        ssidLine[16] = '\0';
    }
    canvas.drawString(ssidLine, centerX, boxY + 6);
    
    // BSSID (already uppercase from storage)
    canvas.drawString(cap.bssid.c_str(), centerX, boxY + 20);
    
    // Status and password
    if (cap.status == CaptureStatus::CRACKED) {
        canvas.drawString("** CR4CK3D **", centerX, boxY + 38);
        
        // Password in larger text
        char pwLine[24];
        const char* pwSrc = cap.password.c_str();
        size_t pwLen = strlen(pwSrc);
        if (pwLen > 20 && sizeof(pwLine) > 20) {
            size_t keep = 18;
            memcpy(pwLine, pwSrc, keep);
            pwLine[keep] = '.';
            pwLine[keep + 1] = '.';
            pwLine[keep + 2] = '\0';
        } else {
            strncpy(pwLine, pwSrc, sizeof(pwLine) - 1);
            pwLine[sizeof(pwLine) - 1] = '\0';
        }
        canvas.drawString(pwLine, centerX, boxY + 54);
    } else if (cap.status == CaptureStatus::UPLOADED) {
        canvas.drawString("UPLOADED, WAITING...", centerX, boxY + 38);
        canvas.drawString("[R] REFRESH RESULTS", centerX, boxY + 54);
    } else if (cap.isPMKID) {
        canvas.drawString("PMKID - LOCAL CRACK ONLY", centerX, boxY + 38);
        canvas.drawString("hashcat -m 22000", centerX, boxY + 54);
    } else {
        canvas.drawString("NOT UPLOADED YET", centerX, boxY + 38);
        canvas.drawString("[U] UPLOAD TO WPA-SEC", centerX, boxY + 54);
    }
    
    canvas.drawString("[ENTER] CLOSE", centerX, boxY + 72);
}

void CapturesMenu::drawConnecting(M5Canvas& canvas) {
    // Overlay message
    const int boxW = 180;
    const int boxH = 40;
    const int boxX = (canvas.width() - boxW) / 2;
    const int boxY = (canvas.height() - boxH) / 2;
    
    canvas.fillRoundRect(boxX - 2, boxY - 2, boxW + 4, boxH + 4, 6, COLOR_BG);
    canvas.fillRoundRect(boxX, boxY, boxW, boxH, 6, COLOR_FG);
    
    canvas.setTextColor(COLOR_BG, COLOR_FG);
    canvas.setTextDatum(top_center);
    
    int centerX = canvas.width() / 2;
    
    if (connectingWiFi) {
        canvas.drawString("CONNECTING WIFI...", centerX, boxY + 8);
        canvas.drawString(WPASec::getStatus(), centerX, boxY + 22);
    } else if (uploadingFile) {
        canvas.drawString("UPLOADING...", centerX, boxY + 8);
        canvas.drawString(WPASec::getStatus(), centerX, boxY + 22);
    } else if (refreshingResults) {
        canvas.drawString("FETCHING RESULTS...", centerX, boxY + 8);
        canvas.drawString(WPASec::getStatus(), centerX, boxY + 22);
    }
}

void CapturesMenu::uploadSelected() {
    if (selectedIndex >= captures.size()) return;

    // Prevent starting multiple parallel WPA‑SEC operations
    if (wpaTaskHandle != nullptr) {
        Display::setTopBarMessage("WPA-SEC BUSY", 3000);
        return;
    }
    // Guard: ensure enough contiguous heap for task stack (~24 KB)
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 30000) {
        Display::setTopBarMessage("LOW HEAP FOR WPA-SEC", 3000);
        return;
    }

    const CaptureInfo& cap = captures[selectedIndex];

    // Check if WPA-SEC key is configured
    if (Config::wifi().wpaSecKey.isEmpty()) {
        Display::setTopBarMessage("SET WPA-SEC KEY FIRST", 4000);
        return;
    }

    // Already cracked? No need to upload
    if (cap.status == CaptureStatus::CRACKED) {
        Display::setTopBarMessage("ALREADY CRACKED", 3000);
        return;
    }

    // Already uploaded? Hard-stop for messaging parity.
    if (WPASec::isUploaded(cap.bssid.c_str())) {
        Display::setTopBarMessage("ALREADY UPLOADED", 3000);
        return;
    }

    // PMKIDs can't be uploaded (WPA-SEC requires PCAP format)
    if (cap.isPMKID) {
        Display::setTopBarMessage("PMKID = LOCAL ONLY", 4000);
        return;
    }

    // Find the PCAP file for this capture
    String baseName = cap.bssid;
    baseName.replace(":", "");
    String pcapPath = "/handshakes/" + baseName + ".pcap";

    if (!SD.exists(pcapPath)) {
        Display::setTopBarMessage("NO PCAP FILE FOUND", 4000);
        return;
    }

    // Kick off upload in a dedicated FreeRTOS task with a larger stack.
    // TLS handshakes can overflow Arduino's loopTask stack.
    wpaTaskDone = false;
    wpaTaskSuccess = false;
    wpaTaskAction = WpaTaskAction::UPLOAD;
    wpaTaskIndex = selectedIndex;
    wpaTaskResultMsg[0] = '\0';

    WpaTaskCtx* ctx = new WpaTaskCtx();
    ctx->action = WpaTaskAction::UPLOAD;
    strncpy(ctx->pcapPath, pcapPath.c_str(), sizeof(ctx->pcapPath) - 1);
    ctx->pcapPath[sizeof(ctx->pcapPath) - 1] = '\0';
    ctx->index = selectedIndex;

    uploadingFile = true;
    Display::setTopBarMessage("WPA-SEC UP...", 0);

    BaseType_t ok = xTaskCreatePinnedToCore(
        wpaTaskFn,
        "wpasec_upload",
        16384,
        ctx,
        1,
        &wpaTaskHandle,
        0
    );
    if (ok != pdPASS) {
        wpaTaskHandle = nullptr;
        uploadingFile = false;
        delete ctx;
        Display::setTopBarMessage("WPA-SEC TASK FAIL", 4000);
    }
}

void CapturesMenu::refreshResults() {
    // Check if WPA-SEC key is configured
    if (Config::wifi().wpaSecKey.isEmpty()) {
        Display::setTopBarMessage("SET WPA-SEC KEY FIRST", 4000);
        return;
    }

    // Prevent starting multiple parallel WPA‑SEC operations
    if (wpaTaskHandle != nullptr) {
        Display::setTopBarMessage("WPA-SEC BUSY", 3000);
        return;
    }
    // Guard: ensure enough contiguous heap for task stack (~24 KB)
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < 30000) {
        Display::setTopBarMessage("LOW HEAP FOR WPA-SEC", 3000);
        return;
    }

    // Kick off fetch in a dedicated FreeRTOS task with a larger stack.
    wpaTaskDone = false;
    wpaTaskSuccess = false;
    wpaTaskAction = WpaTaskAction::REFRESH;
    wpaTaskIndex = selectedIndex;
    wpaTaskResultMsg[0] = '\0';

    WpaTaskCtx* ctx = new WpaTaskCtx();
    ctx->action = WpaTaskAction::REFRESH;
    ctx->pcapPath[0] = '\0';
    ctx->index = selectedIndex;

    refreshingResults = true;
    Display::setTopBarMessage("WPA-SEC FETCH...", 0);

    BaseType_t ok = xTaskCreatePinnedToCore(
        wpaTaskFn,
        "wpasec_fetch",
        16384,
        ctx,
        1,
        &wpaTaskHandle,
        0
    );
    if (ok != pdPASS) {
        wpaTaskHandle = nullptr;
        refreshingResults = false;
        delete ctx;
        Display::setTopBarMessage("WPA-SEC TASK FAIL", 4000);
    }
}
