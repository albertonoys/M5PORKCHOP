// Captures Menu - View saved handshake captures

#include "captures_menu.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <WiFi.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include "display.h"
#include "../web/wpasec.h"
#include "../core/config.h"
#include "../core/sd_layout.h"
#include "../core/wifi_utils.h"

// Static member initialization
std::vector<CaptureInfo> CapturesMenu::captures;
uint8_t CapturesMenu::selectedIndex = 0;
uint8_t CapturesMenu::scrollOffset = 0;
bool CapturesMenu::active = false;
bool CapturesMenu::keyWasPressed = false;
bool CapturesMenu::nukeConfirmActive = false;
bool CapturesMenu::detailViewActive = false;
bool CapturesMenu::scanInProgress = false;
unsigned long CapturesMenu::lastScanTime = 0;
File CapturesMenu::scanDir;
File CapturesMenu::currentFile;
bool CapturesMenu::scanComplete = false;
size_t CapturesMenu::scanProgress = 0;
bool CapturesMenu::wpasecUpdateInProgress = false;
unsigned long CapturesMenu::lastWpasecUpdateTime = 0;
size_t CapturesMenu::wpasecUpdateProgress = 0;

// WPA-SEC Sync state
bool CapturesMenu::syncModalActive = false;
SyncState CapturesMenu::syncState = SyncState::IDLE;
char CapturesMenu::syncStatusText[48] = "";
uint8_t CapturesMenu::syncProgress = 0;
uint8_t CapturesMenu::syncTotal = 0;
unsigned long CapturesMenu::syncStartTime = 0;
uint8_t CapturesMenu::syncUploaded = 0;
uint8_t CapturesMenu::syncFailed = 0;
uint16_t CapturesMenu::syncCracked = 0;
char CapturesMenu::syncError[48] = "";

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
    
    // FIX: Always call emergencyCleanup first - ensures file handles closed
    emergencyCleanup();
    
    // Enhanced: Force cleanup even if interrupted
    captures.clear();
    captures.shrink_to_fit();  // Release vector capacity
    WPASec::freeCacheMemory();
    
    // Reset all async state to prevent leaks (redundant after emergencyCleanup but safe)
    scanInProgress = false;
    wpasecUpdateInProgress = false;
    if (scanDir) {
        scanDir.close();
    }
    if (currentFile) {
        currentFile.close();
    }
}

void CapturesMenu::emergencyCleanup() {
    // Can be called from main loop when heap is critical
    if (!active) return;
    
    Serial.println("[CAPTURES] Emergency cleanup triggered");
    captures.clear();
    captures.shrink_to_fit();
    WPASec::freeCacheMemory();
    
    // Stop any in-progress operations
    scanInProgress = false;
    wpasecUpdateInProgress = false;
    if (scanDir) {
        scanDir.close();
    }
    if (currentFile) {
        currentFile.close();
    }
}

bool CapturesMenu::scanCaptures() {
    // Initialize async scan
    captures.clear();
    captures.reserve(MAX_CAPTURES);

    // Guard: Skip if no SD card available
    if (!Config::isSDAvailable()) {
        Serial.println("[CAPTURES] No SD card available");
        scanComplete = true;
        scanInProgress = false;
        return false;
    }

    // Create directory if it doesn't exist
    const char* handshakesDir = SDLayout::handshakesDir();
    if (!SD.exists(handshakesDir)) {
        Serial.println("[CAPTURES] No handshakes directory, creating...");
        if (!SD.mkdir(handshakesDir)) {
            Serial.println("[CAPTURES] Failed to create handshakes directory");
            scanComplete = true;
            scanInProgress = false;
            return false;
        }
    }

    scanDir = SD.open(handshakesDir);
    if (!scanDir || !scanDir.isDirectory()) {
        Serial.println("[CAPTURES] Failed to open handshakes directory");
        scanComplete = true;
        scanInProgress = false;
        scanDir.close();
        return false;
    }

    scanInProgress = true;
    scanComplete = false;
    scanProgress = 0;
    lastScanTime = millis();
    
    return true;
}

void CapturesMenu::processAsyncScan() {
    if (!scanInProgress || scanComplete) {
        return;
    }
    
    // Throttle the scan to avoid blocking the UI
    if (millis() - lastScanTime < SCAN_DELAY) {
        return;
    }
    
    lastScanTime = millis();
    
    // Process a chunk of files
    size_t processed = 0;
    while (processed < SCAN_CHUNK_SIZE && !scanComplete) {
        currentFile = scanDir.openNextFile();
        
        if (!currentFile) {
            // No more files, we're done with scanning
            scanComplete = true;
            scanInProgress = false;
            scanDir.close();
            
            // Sort by capture time (newest first)
            std::sort(captures.begin(), captures.end(), [](const CaptureInfo& a, const CaptureInfo& b) {
                return a.captureTime > b.captureTime;
            });

            // Start async WPA-SEC status update after scanning is complete
            if (!captures.empty()) {
                wpasecUpdateInProgress = true;
                wpasecUpdateProgress = 0;
                lastWpasecUpdateTime = millis();
            }

            Serial.printf("[CAPTURES] Async scan complete. Found %d captures\n", captures.size());
            break;
        }
        
        String name = currentFile.name();
        bool isPCAP = name.endsWith(".pcap");
        bool isPMKID = name.endsWith(".22000") && !name.endsWith("_hs.22000");
        bool isHS22000 = name.endsWith("_hs.22000");

        // Skip PCAP if we have the corresponding _hs.22000 (avoid duplicates).
        // Prefer showing _hs.22000 because it's hashcat-ready.
        if (isPCAP) {
            String baseName = name.substring(0, name.indexOf('.'));
            String hs22kPath = String(SDLayout::handshakesDir()) + "/" + baseName + "_hs.22000";
            if (SD.exists(hs22kPath)) {
                // Close current file and continue to next
                currentFile.close();
                processed++;
                continue;
            }
        }

        if (isPCAP || isPMKID || isHS22000) {
            CaptureInfo info;
            memset(&info, 0, sizeof(info));
            strncpy(info.filename, name.c_str(), sizeof(info.filename) - 1);
            info.fileSize = currentFile.size();
            info.captureTime = currentFile.getLastWrite();
            info.isPMKID = isPMKID;  // Only true for PMKID files

            // Extract BSSID from filename (e.g., "64EEB7208286.pcap" or "64EEB7208286_hs.22000")
            String baseName = name.substring(0, name.indexOf('.'));
            // Handle _hs suffix for handshake 22000 files
            if (baseName.endsWith("_hs")) {
                baseName = baseName.substring(0, baseName.length() - 3);
            }
            if (baseName.length() >= 12) {
                const char* b = baseName.c_str();
                snprintf(info.bssid, sizeof(info.bssid),
                         "%.2s:%.2s:%.2s:%.2s:%.2s:%.2s",
                         b, b+2, b+4, b+6, b+8, b+10);
            } else {
                strncpy(info.bssid, baseName.c_str(), sizeof(info.bssid) - 1);
            }

            // Try to get SSID from companion .txt file if it exists. For PMKID we use _pmkid.txt suffix, otherwise .txt
            String txtPath = isPMKID ?
                (String(SDLayout::handshakesDir()) + "/" + baseName + "_pmkid.txt") :
                (String(SDLayout::handshakesDir()) + "/" + baseName + ".txt");
            if (SD.exists(txtPath)) {
                File txtFile = SD.open(txtPath, FILE_READ);
                if (txtFile) {
                    String line = txtFile.readStringUntil('\n');
                    line.trim();
                    strncpy(info.ssid, line.c_str(), sizeof(info.ssid) - 1);
                    txtFile.close();
                }
            }
            if (info.ssid[0] == '\0') {
                strncpy(info.ssid, "[UNKNOWN]", sizeof(info.ssid) - 1);
            }

            // Default status and password
            info.status = CaptureStatus::LOCAL;
            // password already zeroed by memset

            captures.push_back(info);
            
            // Hard cap at 100 captures to prevent OOM on devices with limited heap
            if (captures.size() >= 100) {
                scanComplete = true;
                scanInProgress = false;
                currentFile.close();
                scanDir.close();
                Serial.println("[CAPTURES] Hit 100 capture limit, stopped scan");
                break;
            }
        }

        // Close current file and increment counter
        currentFile.close();
        processed++;
        scanProgress++;
        
        // Yield periodically to allow other tasks to run
        if (processed >= SCAN_CHUNK_SIZE) {
            // Still more to do, but yield control back to other tasks
            break;
        }
    }
}

void CapturesMenu::updateWPASecStatus() {
    // Load WPA-SEC cache (lazy, only loads once)
    WPASec::loadCache();
    
    char normalized[13] = {0};
    for (auto& cap : captures) {
        // Normalize BSSID for lookup (remove colons)
        WPASec::normalizeBSSID_Char(cap.bssid, normalized, sizeof(normalized));
        if (normalized[0] == '\0') {
            cap.status = CaptureStatus::LOCAL;
            continue;
        }
        
        if (WPASec::isCracked(normalized)) {
            cap.status = CaptureStatus::CRACKED;
            strncpy(cap.password, WPASec::getPassword(normalized), sizeof(cap.password) - 1);
            cap.password[sizeof(cap.password) - 1] = '\0';
        } else if (WPASec::isUploaded(normalized)) {
            cap.status = CaptureStatus::UPLOADED;
        } else {
            cap.status = CaptureStatus::LOCAL;
        }
    }
}

void CapturesMenu::processAsyncWPASecUpdate() {
    if (!wpasecUpdateInProgress || captures.empty()) {
        wpasecUpdateInProgress = false;
        return;
    }
    
    // Throttle the update to avoid blocking the UI
    if (millis() - lastWpasecUpdateTime < WPASEC_UPDATE_DELAY) {
        return;
    }
    
    lastWpasecUpdateTime = millis();
    
    // Process a chunk of captures
    size_t processed = 0;
    while (processed < WPASEC_UPDATE_CHUNK_SIZE && wpasecUpdateProgress < captures.size()) {
        auto& cap = captures[wpasecUpdateProgress];
        
        // Normalize BSSID for lookup (remove colons)
        char normalized[13] = {0};
        WPASec::normalizeBSSID_Char(cap.bssid, normalized, sizeof(normalized));
        
        if (normalized[0] != '\0') {
            if (WPASec::isCracked(normalized)) {
                cap.status = CaptureStatus::CRACKED;
                strncpy(cap.password, WPASec::getPassword(normalized), sizeof(cap.password) - 1);
                cap.password[sizeof(cap.password) - 1] = '\0';
            } else if (WPASec::isUploaded(normalized)) {
                cap.status = CaptureStatus::UPLOADED;
            } else {
                cap.status = CaptureStatus::LOCAL;
            }
        } else {
            cap.status = CaptureStatus::LOCAL;
        }
        
        wpasecUpdateProgress++;
        processed++;
        
        // Yield periodically to allow other tasks to run
        if (processed >= WPASEC_UPDATE_CHUNK_SIZE) {
            // Still more to do, but yield control back to other tasks
            break;
        }
    }
    
    // Check if we're done with all captures
    if (wpasecUpdateProgress >= captures.size()) {
        wpasecUpdateInProgress = false;
        Serial.printf("[CAPTURES] Async WPA-SEC update complete. Updated %d captures\n", captures.size());
    }
}

void CapturesMenu::update() {
    if (!active) return;
    
    // Process sync state machine if active
    if (syncModalActive && syncState != SyncState::IDLE && 
        syncState != SyncState::COMPLETE && syncState != SyncState::ERROR) {
        processSyncState();
    }
    
    // Process async file scanning if in progress (not during sync)
    if (!syncModalActive) {
        processAsyncScan();
        
        // Process async WPA-SEC status updates if in progress
        processAsyncWPASecUpdate();
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

    // Handle sync modal
    if (syncModalActive) {
        if (syncState == SyncState::ERROR || syncState == SyncState::COMPLETE) {
            // Enter closes the modal after completion/error
            if (keys.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                syncModalActive = false;
                syncState = SyncState::IDLE;
                scanCaptures();  // Rescan captures after sync
            }
        } else {
            // ESC cancels during sync
            if (M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
                cancelSync();
            }
        }
        return;  // Block other inputs during sync
    }

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
    
    // Handle detail view modal - Enter/backspace closes
    if (detailViewActive) {
        if (keys.enter || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
            detailViewActive = false;
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
    
    // S key triggers WPA-SEC sync
    if (M5Cardputer.Keyboard.isKeyPressed('s') || M5Cardputer.Keyboard.isKeyPressed('S')) {
        startSync();
    }
    
    // Nuke all loot with D key
    if (M5Cardputer.Keyboard.isKeyPressed('d') || M5Cardputer.Keyboard.isKeyPressed('D')) {
        if (!captures.empty()) {
            nukeConfirmActive = true;
            Display::setBottomOverlay("PERMANENT | NO UNDO");
        }
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
        canvas.print("NO SD CARD");
        canvas.setCursor(4, 55);
        canvas.print("INSERT AND RESTART");
        return;
    }

    // Draw sync modal FIRST - takes precedence over empty captures message
    // (captures are cleared for heap during sync, but modal should still show)
    if (syncModalActive) {
        drawSyncModal(canvas);
        return;
    }

    if (captures.empty()) {
        canvas.setCursor(4, 36);
        canvas.print("NO CAPTURES FOUND");
        canvas.setCursor(4, 52);
        canvas.print("PRESS [O] FOR OINK");
        canvas.setCursor(4, 68);
        canvas.print("SYNC VIA COMMANDER");
        return;
    }

    // Summary line
    uint16_t total = captures.size();
    uint16_t hsCount = 0;
    uint16_t pmkCount = 0;
    uint16_t cracked = 0;
    uint16_t uploaded = 0;
    for (const auto& cap : captures) {
        if (cap.isPMKID) pmkCount++;
        else hsCount++;
        if (cap.status == CaptureStatus::CRACKED) cracked++;
        else if (cap.status == CaptureStatus::UPLOADED) uploaded++;
    }
    char summary[64];
    snprintf(summary, sizeof(summary), "CAP %u HS %u PMK %u CRK %u UP %u",
             (unsigned)total, (unsigned)hsCount, (unsigned)pmkCount,
             (unsigned)cracked, (unsigned)uploaded);
    canvas.setCursor(4, 2);
    canvas.print(summary);

    // Header row
    canvas.setCursor(4, 12);
    canvas.print("SSID");
    canvas.setCursor(105, 12);
    canvas.print("ST");
    canvas.setCursor(135, 12);
    canvas.print("TIME");
    canvas.setCursor(210, 12);
    canvas.print("SIZE");

    // Draw captures list
    int y = 22;
    int lineHeight = 16;

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
        const char* ssidSrc = cap.ssid;
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
        canvas.setCursor(canvas.width() - 10, 22);
        canvas.setTextColor(COLOR_FG);
        canvas.print("^");
    }
    if (scrollOffset + VISIBLE_ITEMS < captures.size()) {
        canvas.setCursor(canvas.width() - 10, 22 + (VISIBLE_ITEMS - 1) * lineHeight);
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
    
    // Draw sync modal if active
    if (syncModalActive) {
        drawSyncModal(canvas);
    }

    // Bottom bar controls via getSelectedBSSID()
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
    char cmd[56];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/*", SDLayout::handshakesDir());
    canvas.drawString(cmd, centerX, boxY + 22);
    canvas.drawString("THIS KILLS THE LOOT.", centerX, boxY + 36);
    canvas.drawString("[Y] DO IT  [N] ABORT", centerX, boxY + 54);
}

void CapturesMenu::nukeLoot() {
    Serial.println("[CAPTURES] Nuking all loot...");
    
    const char* handshakesDir = SDLayout::handshakesDir();
    if (!SD.exists(handshakesDir)) {
        return;
    }

    File dir = SD.open(handshakesDir);
    if (!dir || !dir.isDirectory()) {
        return;
    }
    
    // Batch collect + delete to avoid vector<String> fragmentation
    // Can't delete while iterating SD, so collect batches of 20
    int deleted = 0;
    bool moreFiles = true;
    while (moreFiles) {
        char paths[20][80];
        uint8_t batchCount = 0;

        dir = SD.open(handshakesDir);
        if (!dir) break;

        File file = dir.openNextFile();
        while (file && batchCount < 20) {
            const char* base = file.name();
            const char* slash = strrchr(base, '/');
            const char* name = slash ? slash + 1 : base;
            snprintf(paths[batchCount], sizeof(paths[0]), "%s/%s", handshakesDir, name);
            batchCount++;
            file.close();
            file = dir.openNextFile();
        }
        if (file) file.close();
        dir.close();

        if (batchCount == 0) break;
        moreFiles = (batchCount == 20);  // Might have more

        for (uint8_t i = 0; i < batchCount; i++) {
            if (SD.remove(paths[i])) deleted++;
        }
        yield();
    }
    
    Serial.printf("[CAPTURES] Nuked %d files\n", deleted);
    
    // Reset selection
    selectedIndex = 0;
    scrollOffset = 0;
    captures.clear();
}

const char* CapturesMenu::getSelectedBSSID() {
    return "ENT=DET S=SYNC D=NUKE";
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
    const char* ssidSrc = cap.ssid;
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
    canvas.drawString(cap.bssid, centerX, boxY + 20);
    
    // Status and password
    if (cap.status == CaptureStatus::CRACKED) {
        canvas.drawString("** CR4CK3D **", centerX, boxY + 38);
        
        // Password in larger text
        char pwLine[24];
        const char* pwSrc = cap.password;
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
        canvas.drawString("UPLOADED - PENDING CRACK", centerX, boxY + 38);
        canvas.drawString("PRESS [S] TO CHECK STATUS", centerX, boxY + 54);
    } else if (cap.isPMKID) {
        canvas.drawString("PMKID - LOCAL CRACK ONLY", centerX, boxY + 38);
        canvas.drawString("hashcat -m 22000", centerX, boxY + 54);
    } else {
        canvas.drawString("NOT UPLOADED YET", centerX, boxY + 38);
        canvas.drawString("PRESS [S] TO SYNC", centerX, boxY + 54);
    }
    
    canvas.drawString("[ENTER] CLOSE", centerX, boxY + 72);
}

// ============================================================================
// WPA-SEC Sync Operations
// ============================================================================

void CapturesMenu::onSyncProgress(const char* status, uint8_t progress, uint8_t total) {
    // Update sync state for UI
    strncpy(syncStatusText, status, sizeof(syncStatusText) - 1);
    syncStatusText[sizeof(syncStatusText) - 1] = '\0';
    syncProgress = progress;
    syncTotal = total;
}

bool CapturesMenu::connectToWiFi() {
    const char* ssid = Config::wifi().otaSSID;
    const char* password = Config::wifi().otaPassword;
    
    if (!ssid || ssid[0] == '\0') {
        strncpy(syncError, "NO WIFI SSID CONFIG", sizeof(syncError) - 1);
        return false;
    }
    
    Serial.printf("[CAPTURES] Connecting to WiFi: %s\n", ssid);
    strncpy(syncStatusText, "CONNECTING WIFI...", sizeof(syncStatusText) - 1);
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    unsigned long startTime = millis();
    const unsigned long timeout = 15000;  // 15 second timeout
    
    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < timeout) {
        delay(100);
        yield();
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        strncpy(syncError, "WIFI CONNECT FAILED", sizeof(syncError) - 1);
        // Keep driver alive to avoid esp_wifi_init 257 on fragmented heap.
        WiFiUtils::shutdown();
        return false;
    }
    
    Serial.printf("[CAPTURES] WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

void CapturesMenu::disconnectWiFi() {
    // Keep driver alive to avoid esp_wifi_init 257 on fragmented heap.
    WiFiUtils::shutdown();
    Serial.println("[CAPTURES] WiFi disconnected");
}

void CapturesMenu::startSync() {
    Serial.println("[CAPTURES] Starting WPA-SEC sync...");
    
    // Reset sync state
    syncModalActive = true;
    syncState = SyncState::CONNECTING_WIFI;
    syncStatusText[0] = '\0';
    syncError[0] = '\0';
    syncProgress = 0;
    syncTotal = 0;
    syncUploaded = 0;
    syncFailed = 0;
    syncCracked = 0;
    syncStartTime = millis();
    
    // Pre-flight checks
    if (!WPASec::hasApiKey()) {
        strncpy(syncError, "NO WPA-SEC KEY", sizeof(syncError) - 1);
        syncState = SyncState::ERROR;
        return;
    }
    
    // Free memory before heavy operations
    captures.clear();
    captures.shrink_to_fit();
    WPASec::freeCacheMemory();
    
    Serial.printf("[CAPTURES] Heap after freeing: %u\n", (unsigned int)ESP.getFreeHeap());
}

void CapturesMenu::cancelSync() {
    Serial.println("[CAPTURES] Sync cancelled");
    
    // Clean up
    disconnectWiFi();
    syncModalActive = false;
    syncState = SyncState::IDLE;
    
    // Rescan captures
    scanCaptures();
}

void CapturesMenu::processSyncState() {
    if (!syncModalActive || syncState == SyncState::IDLE) {
        return;
    }
    
    switch (syncState) {
        case SyncState::CONNECTING_WIFI:
            strncpy(syncStatusText, "CONNECTING WIFI...", sizeof(syncStatusText) - 1);
            if (connectToWiFi()) {
                syncState = SyncState::FREEING_MEMORY;
            } else {
                syncState = SyncState::ERROR;
            }
            break;
            
        case SyncState::FREEING_MEMORY:
            strncpy(syncStatusText, "PREPARING...", sizeof(syncStatusText) - 1);
            // Defer heap gating to WPASec::syncCaptures() so conditioning can run.
            syncState = SyncState::UPLOADING;
            break;
            
        case SyncState::UPLOADING:
            {
                // Run sync (blocking but with progress callback)
                strncpy(syncStatusText, "SYNCING...", sizeof(syncStatusText) - 1);
                
                WPASecSyncResult result = WPASec::syncCaptures(onSyncProgress);
                
                syncUploaded = result.uploaded;
                syncFailed = result.failed;
                syncCracked = result.cracked;
                
                if (result.error[0] != '\0') {
                    strncpy(syncError, result.error, sizeof(syncError) - 1);
                }
                
                syncState = SyncState::COMPLETE;
            }
            break;
            
        case SyncState::DOWNLOADING_POTFILE:
            // Handled within UPLOADING state via syncCaptures
            break;
            
        case SyncState::COMPLETE:
            // Stay in complete state until user dismisses
            disconnectWiFi();
            break;
            
        case SyncState::ERROR:
            // Stay in error state until user dismisses
            disconnectWiFi();
            break;
            
        default:
            break;
    }
}

void CapturesMenu::drawSyncModal(M5Canvas& canvas) {
    // Modal box dimensions
    const int boxW = 200;
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
    
    // Title
    canvas.drawString("WPA-SEC SYNC", centerX, boxY + 6);
    
    if (syncState == SyncState::ERROR) {
        // Error state
        canvas.drawString("!! ERROR !!", centerX, boxY + 24);
        canvas.drawString(syncError, centerX, boxY + 42);
        canvas.drawString("[ENTER] CLOSE", centerX, boxY + 68);
    } else if (syncState == SyncState::COMPLETE) {
        // Complete state
        canvas.drawString("SYNC COMPLETE", centerX, boxY + 24);
        
        char stats[48];
        snprintf(stats, sizeof(stats), "UP:%u FAIL:%u CRACK:%u", 
                 (unsigned)syncUploaded, (unsigned)syncFailed, (unsigned)syncCracked);
        canvas.drawString(stats, centerX, boxY + 42);
        
        if (syncError[0] != '\0') {
            canvas.drawString(syncError, centerX, boxY + 54);
        }
        
        canvas.drawString("[ENTER] CLOSE", centerX, boxY + 68);
    } else {
        // In progress
        canvas.drawString(syncStatusText, centerX, boxY + 24);
        
        // Progress bar
        if (syncTotal > 0) {
            const int barW = 160;
            const int barH = 10;
            const int barX = boxX + (boxW - barW) / 2;
            const int barY = boxY + 42;
            
            // Background
            canvas.fillRect(barX, barY, barW, barH, COLOR_BG);
            
            // Fill
            int fillW = (barW * syncProgress) / syncTotal;
            if (fillW > 0) {
                canvas.fillRect(barX, barY, fillW, barH, COLOR_FG);
            }
            
            // Progress text
            char progText[16];
            snprintf(progText, sizeof(progText), "%u/%u", (unsigned)syncProgress, (unsigned)syncTotal);
            canvas.drawString(progText, centerX, barY + barH + 4);
        } else {
            // Heap display
            char heapText[32];
            snprintf(heapText, sizeof(heapText), "HEAP: %uKB", 
                     (unsigned)(ESP.getFreeHeap() / 1024));
            canvas.drawString(heapText, centerX, boxY + 42);
        }
        
        canvas.drawString("[ESC] CANCEL", centerX, boxY + 68);
    }
}
