// DO NO HAM Mode implementation
// "BRAVO 6, GOING DARK"
// Passive WiFi reconnaissance - no attacks, just listening

#include "donoham.h"
#include <M5Unified.h>
#include <WiFi.h>
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../core/xp.h"
#include "../core/wsl_bypasser.h"
#include "../ui/display.h"
#include "../piglet/mood.h"
#include "../piglet/avatar.h"
#include <SD.h>

// Static member initialization
bool DoNoHamMode::running = false;
DNHState DoNoHamMode::state = DNHState::HOPPING;
uint8_t DoNoHamMode::currentChannel = 1;
uint8_t DoNoHamMode::channelIndex = 0;
uint32_t DoNoHamMode::lastHopTime = 0;
uint32_t DoNoHamMode::dwellStartTime = 0;
bool DoNoHamMode::dwellResolved = false;

std::vector<DetectedNetwork> DoNoHamMode::networks;
std::vector<CapturedPMKID> DoNoHamMode::pmkids;
std::vector<CapturedHandshake> DoNoHamMode::handshakes;

// Guard flag for race condition prevention
static volatile bool dnhBusy = false;

// Single-slot deferred network add (same pattern as OINK)
static volatile bool pendingNetworkAdd = false;
static DetectedNetwork pendingNetwork;

// Single-slot deferred PMKID create
static volatile bool pendingPMKIDCreateReady = false;
static volatile bool pendingPMKIDCreateBusy = false;
struct PendingPMKIDCreate {
    uint8_t bssid[6];
    uint8_t station[6];
    uint8_t pmkid[16];
    char ssid[33];
    uint8_t channel;
};
static PendingPMKIDCreate pendingPMKIDCreate;

// Channel order: 1, 6, 11 first (non-overlapping), then fill in
static const uint8_t CHANNEL_ORDER[] = {1, 6, 11, 2, 7, 12, 3, 8, 13, 4, 9, 5, 10};

// Timing
static uint32_t lastCleanupTime = 0;
static uint32_t lastSaveTime = 0;
static uint32_t lastMoodTime = 0;

void DoNoHamMode::init() {
    Serial.println("[DNH] Initialized");
}

void DoNoHamMode::start() {
    if (running) return;
    
    Serial.println("[DNH] Starting passive mode");
    SDLog::log("DNH", "Starting passive mode");
    
    // Clear previous session data
    networks.clear();
    networks.shrink_to_fit();
    pmkids.clear();
    pmkids.shrink_to_fit();
    handshakes.clear();
    handshakes.shrink_to_fit();
    
    // Reset state
    state = DNHState::HOPPING;
    channelIndex = 0;
    currentChannel = CHANNEL_ORDER[0];
    lastHopTime = millis();
    lastCleanupTime = millis();
    lastSaveTime = millis();
    lastMoodTime = millis();
    dwellResolved = false;
    
    // Reset deferred flags
    pendingNetworkAdd = false;
    pendingPMKIDCreateReady = false;
    pendingPMKIDCreateBusy = false;
    
    // Randomize MAC if configured
    if (Config::wifi().randomizeMAC) {
        WSLBypasser::randomizeMAC();
    }
    
    // Initialize WiFi in promiscuous mode
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);
    
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_start();
    delay(50);
    
    // Set channel
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    // Enable promiscuous mode with shared callback (OINK's callback dispatches to us)
    esp_wifi_set_promiscuous_rx_cb(OinkMode::promiscuousCallback);
    esp_wifi_set_promiscuous(true);
    
    running = true;
    
    // UI feedback
    Display::showToast("BRAVO 6, GOING DARK");
    Avatar::setState(AvatarState::NEUTRAL);  // Calm, passive state
    Mood::onPassiveRecon(networks.size(), currentChannel);
    
    Serial.printf("[DNH] Started on channel %d\n", currentChannel);
}

void DoNoHamMode::startSeamless() {
    if (running) return;
    
    Serial.println("[DNH] Seamless start (preserving WiFi state)");
    SDLog::log("DNH", "Seamless start");
    
    // DON'T clear vectors - let old data age out naturally
    // DON'T restart promiscuous mode - already running
    // DON'T reset channel - preserve current
    
    // Reset state machine
    state = DNHState::HOPPING;
    lastHopTime = millis();
    lastCleanupTime = millis();
    lastSaveTime = millis();
    lastMoodTime = millis();
    dwellResolved = false;
    
    // Reset deferred flags
    pendingNetworkAdd = false;
    pendingPMKIDCreateReady = false;
    pendingPMKIDCreateBusy = false;
    
    running = true;
    
    // UI feedback
    Display::showToast("BRAVO 6, GOING DARK");
    Avatar::setState(AvatarState::NEUTRAL);  // Calm, passive state
    Mood::onPassiveRecon(networks.size(), currentChannel);
}

void DoNoHamMode::stop() {
    if (!running) return;
    
    Serial.println("[DNH] Stopping");
    SDLog::log("DNH", "Stopping");
    
    running = false;
    
    // Disable promiscuous mode
    esp_wifi_set_promiscuous(false);
    
    // Save any unsaved data
    saveAllPMKIDs();
    saveAllHandshakes();
    
    // Clear vectors
    dnhBusy = true;
    networks.clear();
    networks.shrink_to_fit();
    pmkids.clear();
    pmkids.shrink_to_fit();
    handshakes.clear();
    handshakes.shrink_to_fit();
    dnhBusy = false;
    
    // Reset deferred flags
    pendingNetworkAdd = false;
    pendingPMKIDCreateReady = false;
    pendingPMKIDCreateBusy = false;
    
    Serial.println("[DNH] Stopped");
}

void DoNoHamMode::stopSeamless() {
    if (!running) return;
    
    Serial.println("[DNH] Seamless stop (preserving WiFi state)");
    SDLog::log("DNH", "Seamless stop");
    
    running = false;
    
    // DON'T disable promiscuous mode - OINK will take over
    // DON'T clear vectors - let them die naturally
    
    // Save any unsaved data
    saveAllPMKIDs();
    saveAllHandshakes();
}

void DoNoHamMode::update() {
    if (!running) return;
    
    uint32_t now = millis();
    
    // Set busy flag for race protection
    dnhBusy = true;
    
    // Process deferred network add
    if (pendingNetworkAdd) {
        if (networks.size() < DNH_MAX_NETWORKS) {
            // Check if already exists
            int idx = findNetwork(pendingNetwork.bssid);
            if (idx >= 0) {
                // Update existing
                networks[idx].rssi = pendingNetwork.rssi;
                networks[idx].lastSeen = pendingNetwork.lastSeen;
                networks[idx].beaconCount++;
            } else {
                // Add new
                networks.push_back(pendingNetwork);
                XP::addXP(XPEvent::DNH_NETWORK_PASSIVE);
            }
        }
        pendingNetworkAdd = false;
    }
    
    // Process deferred PMKID create
    if (pendingPMKIDCreateReady && !pendingPMKIDCreateBusy) {
        // Check if dwell is complete (if we needed one)
        bool canProcess = true;
        if (pendingPMKIDCreate.ssid[0] == 0 && state == DNHState::DWELLING) {
            // Still dwelling, wait for beacon or timeout
            if (!dwellResolved && (now - dwellStartTime < DNH_DWELL_TIME)) {
                canProcess = false;
            }
        }
        
        if (canProcess) {
            pendingPMKIDCreateBusy = true;
            
            // Try to find SSID if we don't have it
            if (pendingPMKIDCreate.ssid[0] == 0) {
                int netIdx = findNetwork(pendingPMKIDCreate.bssid);
                if (netIdx >= 0 && networks[netIdx].ssid[0] != 0) {
                    strncpy(pendingPMKIDCreate.ssid, networks[netIdx].ssid, 32);
                    pendingPMKIDCreate.ssid[32] = 0;
                }
            }
            
            // Create or update PMKID entry
            if (pmkids.size() < DNH_MAX_PMKIDS) {
                int idx = findOrCreatePMKID(pendingPMKIDCreate.bssid);
                if (idx >= 0) {
                    memcpy(pmkids[idx].pmkid, pendingPMKIDCreate.pmkid, 16);
                    memcpy(pmkids[idx].station, pendingPMKIDCreate.station, 6);
                    strncpy(pmkids[idx].ssid, pendingPMKIDCreate.ssid, 32);
                    pmkids[idx].ssid[32] = 0;
                    pmkids[idx].timestamp = now;
                    
                    // Announce capture
                    if (pendingPMKIDCreate.ssid[0] != 0) {
                        Serial.printf("[DNH] PMKID captured: %s\n", pendingPMKIDCreate.ssid);
                        Display::showToast("GHOST PMKID!");
                        M5.Speaker.tone(880, 100);
                        delay(50);
                        M5.Speaker.tone(1100, 100);
                        delay(50);
                        M5.Speaker.tone(1320, 100);
                        XP::addXP(XPEvent::DNH_PMKID_GHOST);
                        Mood::onPMKIDCaptured();
                    } else {
                        Serial.println("[DNH] PMKID captured but SSID unknown");
                    }
                }
            }
            
            pendingPMKIDCreateReady = false;
            pendingPMKIDCreateBusy = false;
            
            // Return to hopping if we were dwelling
            if (state == DNHState::DWELLING) {
                state = DNHState::HOPPING;
                dwellResolved = false;
            }
        }
    }
    
    // Channel hopping state machine
    switch (state) {
        case DNHState::HOPPING:
            if (now - lastHopTime > DNH_HOP_INTERVAL) {
                hopToNextChannel();
                lastHopTime = now;
            }
            break;
            
        case DNHState::DWELLING:
            if (dwellResolved || (now - dwellStartTime > DNH_DWELL_TIME)) {
                state = DNHState::HOPPING;
                dwellResolved = false;
            }
            break;
    }
    
    // Periodic cleanup (every 10 seconds)
    if (now - lastCleanupTime > 10000) {
        ageOutStaleNetworks();
        lastCleanupTime = now;
    }
    
    // Periodic save (every 2 seconds)
    if (now - lastSaveTime > 2000) {
        saveAllPMKIDs();
        lastSaveTime = now;
    }
    
    // Mood update (every 3 seconds)
    if (now - lastMoodTime > 3000) {
        Mood::onPassiveRecon(networks.size(), currentChannel);
        lastMoodTime = now;
    }
    
    dnhBusy = false;
}

void DoNoHamMode::hopToNextChannel() {
    channelIndex = (channelIndex + 1) % 13;
    currentChannel = CHANNEL_ORDER[channelIndex];
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

void DoNoHamMode::startDwell() {
    state = DNHState::DWELLING;
    dwellStartTime = millis();
    dwellResolved = false;
    Serial.printf("[DNH] Dwelling on ch %d for SSID\n", currentChannel);
}

void DoNoHamMode::ageOutStaleNetworks() {
    uint32_t now = millis();
    auto it = networks.begin();
    while (it != networks.end()) {
        if (now - it->lastSeen > DNH_STALE_TIMEOUT) {
            it = networks.erase(it);
        } else {
            ++it;
        }
    }
}

void DoNoHamMode::saveAllPMKIDs() {
    // Save PMKIDs in hashcat 22000 format
    for (auto& p : pmkids) {
        if (p.saved) continue;
        
        // Try to backfill SSID if missing
        if (p.ssid[0] == 0) {
            int netIdx = findNetwork(p.bssid);
            if (netIdx >= 0 && networks[netIdx].ssid[0] != 0) {
                strncpy(p.ssid, networks[netIdx].ssid, 32);
                p.ssid[32] = 0;
            }
        }
        
        // Can only save if we have SSID
        if (p.ssid[0] == 0) continue;
        
        // Check for all-zero PMKID (invalid)
        bool allZeros = true;
        for (int i = 0; i < 16; i++) {
            if (p.pmkid[i] != 0) { allZeros = false; break; }
        }
        if (allZeros) continue;
        
        // Build filename: /handshakes/BSSID.22000
        char filename[64];
        snprintf(filename, sizeof(filename), "/handshakes/%02X%02X%02X%02X%02X%02X.22000",
            p.bssid[0], p.bssid[1], p.bssid[2], p.bssid[3], p.bssid[4], p.bssid[5]);
        
        // Ensure directory exists
        if (!SD.exists("/handshakes")) {
            SD.mkdir("/handshakes");
        }
        
        File f = SD.open(filename, FILE_WRITE);
        if (!f) {
            Serial.printf("[DNH] Failed to create PMKID file: %s\n", filename);
            continue;
        }
        
        // Build hex strings
        char pmkidHex[33];
        for (int i = 0; i < 16; i++) {
            sprintf(pmkidHex + i*2, "%02x", p.pmkid[i]);
        }
        
        char macAP[13];
        sprintf(macAP, "%02x%02x%02x%02x%02x%02x",
            p.bssid[0], p.bssid[1], p.bssid[2], 
            p.bssid[3], p.bssid[4], p.bssid[5]);
        
        char macClient[13];
        sprintf(macClient, "%02x%02x%02x%02x%02x%02x",
            p.station[0], p.station[1], p.station[2], 
            p.station[3], p.station[4], p.station[5]);
        
        char essidHex[65];
        int ssidLen = strlen(p.ssid);
        for (int i = 0; i < ssidLen && i < 32; i++) {
            sprintf(essidHex + i*2, "%02x", (uint8_t)p.ssid[i]);
        }
        essidHex[ssidLen * 2] = 0;
        
        // WPA*01*PMKID*MAC_AP*MAC_CLIENT*ESSID***01
        f.printf("WPA*01*%s*%s*%s*%s***01\n", pmkidHex, macAP, macClient, essidHex);
        f.close();
        
        p.saved = true;
        Serial.printf("[DNH] PMKID saved: %s\n", filename);
        SDLog::log("DNH", "PMKID saved: %s (%s)", p.ssid, filename);
    }
}

void DoNoHamMode::saveAllHandshakes() {
    // TODO: Implement handshake saving to SD card
    // Format: hashcat 22000 WPA*02 format
}

int DoNoHamMode::findNetwork(const uint8_t* bssid) {
    for (size_t i = 0; i < networks.size(); i++) {
        if (memcmp(networks[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    return -1;
}

int DoNoHamMode::findOrCreatePMKID(const uint8_t* bssid) {
    // Find existing
    for (size_t i = 0; i < pmkids.size(); i++) {
        if (memcmp(pmkids[i].bssid, bssid, 6) == 0) {
            return i;
        }
    }
    // Create new
    if (pmkids.size() < DNH_MAX_PMKIDS) {
        CapturedPMKID p = {};
        memcpy(p.bssid, bssid, 6);
        pmkids.push_back(p);
        return pmkids.size() - 1;
    }
    return -1;
}

// Frame handlers - called from shared promiscuous callback
void DoNoHamMode::handleBeacon(const uint8_t* frame, uint16_t len, int8_t rssi) {
    if (!running) return;
    if (dnhBusy) return;  // Skip if update() is processing vectors
    
    // Beacon frame structure:
    // [0-1] Frame Control, [2-3] Duration, [4-9] DA, [10-15] SA (BSSID), [16-21] BSSID
    // [22-23] Seq, [24-35] Timestamp, [36-37] Beacon Interval, [38-39] Capability
    // [40+] IEs
    
    if (len < 40) return;
    
    const uint8_t* bssid = frame + 16;
    
    // Parse SSID from IE 0
    char ssid[33] = {0};
    uint16_t offset = 36;  // Start of fixed fields after header
    offset += 12;  // Skip timestamp(8) + beacon_interval(2) + capability(2)
    
    while (offset + 2 < len) {
        uint8_t ieType = frame[offset];
        uint8_t ieLen = frame[offset + 1];
        if (offset + 2 + ieLen > len) break;
        
        if (ieType == 0 && ieLen > 0 && ieLen <= 32) {
            memcpy(ssid, frame + offset + 2, ieLen);
            ssid[ieLen] = 0;
            break;
        }
        offset += 2 + ieLen;
    }
    
    // Check if this resolves a pending PMKID dwell
    if (state == DNHState::DWELLING && ssid[0] != 0) {
        if (memcmp(bssid, pendingPMKIDCreate.bssid, 6) == 0) {
            strncpy(pendingPMKIDCreate.ssid, ssid, 32);
            pendingPMKIDCreate.ssid[32] = 0;
            dwellResolved = true;
            Serial.printf("[DNH] Dwell resolved: %s\n", ssid);
        }
    }
    
    // Queue network for deferred add
    if (!pendingNetworkAdd) {
        memset(&pendingNetwork, 0, sizeof(pendingNetwork));
        memcpy(pendingNetwork.bssid, bssid, 6);
        strncpy(pendingNetwork.ssid, ssid, 32);
        pendingNetwork.ssid[32] = 0;
        pendingNetwork.rssi = rssi;
        pendingNetwork.channel = currentChannel;
        pendingNetwork.lastSeen = millis();
        pendingNetwork.beaconCount = 1;
        pendingNetworkAdd = true;
    }
}

void DoNoHamMode::handleEAPOL(const uint8_t* frame, uint16_t len, int8_t rssi) {
    if (!running) return;
    if (dnhBusy) return;  // Skip if update() is processing vectors
    
    // Parse 802.11 data frame to find EAPOL
    // Frame: FC(2) + Duration(2) + Addr1(6) + Addr2(6) + Addr3(6) + Seq(2) = 24 bytes
    // Then QoS(2) if present, then LLC/SNAP(8), then EAPOL payload
    
    if (len < 24) return;
    
    // Check To/From DS flags
    uint8_t toDs = (frame[1] & 0x01);
    uint8_t fromDs = (frame[1] & 0x02) >> 1;
    
    // Extract MACs based on To/From DS
    const uint8_t* srcMac;
    const uint8_t* dstMac;
    const uint8_t* bssid;
    
    if (toDs && !fromDs) {
        // To DS: RA=BSSID, TA=SA
        dstMac = frame + 4;
        srcMac = frame + 10;
        bssid = frame + 4;
    } else if (!toDs && fromDs) {
        // From DS: RA=DA, TA=BSSID
        dstMac = frame + 4;
        srcMac = frame + 10;
        bssid = frame + 10;
    } else if (!toDs && !fromDs) {
        // IBSS or Direct Link
        dstMac = frame + 4;
        srcMac = frame + 10;
        bssid = frame + 16;
    } else {
        // WDS (both set) - skip
        return;
    }
    
    // Calculate data offset (after 802.11 header)
    uint16_t offset = 24;
    
    // Check for QoS Data (subtype bit 3 set)
    uint8_t subtype = (frame[0] >> 4) & 0x0F;
    bool isQoS = (subtype & 0x08) != 0;
    if (isQoS) offset += 2;
    
    // Check for HTC (QoS + Order bit)
    if (isQoS && (frame[1] & 0x80)) offset += 4;
    
    if (offset + 8 > len) return;
    
    // Check LLC/SNAP for EAPOL: AA AA 03 00 00 00 88 8E
    if (frame[offset] != 0xAA || frame[offset+1] != 0xAA ||
        frame[offset+2] != 0x03 || frame[offset+3] != 0x00 ||
        frame[offset+4] != 0x00 || frame[offset+5] != 0x00 ||
        frame[offset+6] != 0x88 || frame[offset+7] != 0x8E) {
        return;  // Not EAPOL
    }
    
    // EAPOL payload starts after LLC/SNAP
    const uint8_t* eapol = frame + offset + 8;
    uint16_t eapolLen = len - offset - 8;
    
    if (eapolLen < 4) return;
    
    // EAPOL: version(1) + type(1) + length(2)
    uint8_t eapolType = eapol[1];
    if (eapolType != 3) return;  // EAPOL-Key only
    
    if (eapolLen < 99) return;  // Minimum for key frame
    
    // EAPOL-Key: descriptor_type(1) @ 4, key_info(2) @ 5-6
    uint16_t keyInfo = (eapol[5] << 8) | eapol[6];
    bool keyAck = (keyInfo & 0x0080) != 0;
    bool keyMic = (keyInfo & 0x0100) != 0;
    bool secure = (keyInfo & 0x0200) != 0;
    
    // Identify message: M1 = KeyAck, no MIC, no Secure
    uint8_t messageNum = 0;
    if (keyAck && !keyMic && !secure) messageNum = 1;
    else if (!keyAck && keyMic && !secure) messageNum = 2;
    else if (keyAck && keyMic && secure) messageNum = 3;
    else if (!keyAck && keyMic && secure) messageNum = 4;
    
    // We only care about M1 for PMKID
    if (messageNum != 1) return;
    
    // Determine BSSID and station from M1 (AP->Station)
    uint8_t apBssid[6], station[6];
    memcpy(apBssid, srcMac, 6);
    memcpy(station, dstMac, 6);
    
    // ========== PMKID EXTRACTION FROM M1 ==========
    // Descriptor type 0x02 = RSN (WPA2/WPA3), 0xFE = WPA1
    uint8_t descriptorType = eapol[4];
    if (descriptorType != 0x02) return;  // WPA1 doesn't have PMKID
    
    // Key data length at offset 97-98, key data at 99
    if (eapolLen < 121) return;  // Need at least 99 + 22 bytes
    
    uint16_t keyDataLen = (eapol[97] << 8) | eapol[98];
    if (keyDataLen < 22 || eapolLen < 99 + keyDataLen) return;
    
    const uint8_t* keyData = eapol + 99;
    
    // Search for PMKID KDE: dd 14 00 0f ac 04 [16-byte PMKID]
    for (uint16_t i = 0; i + 22 <= keyDataLen; i++) {
        if (keyData[i] == 0xdd && keyData[i+1] == 0x14 &&
            keyData[i+2] == 0x00 && keyData[i+3] == 0x0f &&
            keyData[i+4] == 0xac && keyData[i+5] == 0x04) {
            
            const uint8_t* pmkidData = keyData + i + 6;
            
            // Skip all-zero PMKIDs (invalid)
            bool allZeros = true;
            for (int z = 0; z < 16; z++) {
                if (pmkidData[z] != 0) { allZeros = false; break; }
            }
            if (allZeros) {
                Serial.println("[DNH] PMKID KDE found but all zeros (ignored)");
                break;
            }
            
            // Queue PMKID for creation in main thread
            if (!pendingPMKIDCreateBusy && !pendingPMKIDCreateReady) {
                memcpy(pendingPMKIDCreate.bssid, apBssid, 6);
                memcpy(pendingPMKIDCreate.station, station, 6);
                memcpy(pendingPMKIDCreate.pmkid, pmkidData, 16);
                
                // Try to get SSID from known networks
                int netIdx = findNetwork(apBssid);
                if (netIdx >= 0 && networks[netIdx].ssid[0] != 0) {
                    strncpy(pendingPMKIDCreate.ssid, networks[netIdx].ssid, 32);
                    pendingPMKIDCreate.ssid[32] = 0;
                } else {
                    // No SSID - trigger dwell to catch beacon
                    pendingPMKIDCreate.ssid[0] = 0;
                    state = DNHState::DWELLING;
                    dwellStartTime = millis();
                    dwellResolved = false;
                    Serial.println("[DNH] PMKID needs SSID - dwelling for beacon");
                }
                
                pendingPMKIDCreateReady = true;
                Serial.printf("[DNH] PMKID queued from %02X:%02X:%02X:%02X:%02X:%02X\n",
                    apBssid[0], apBssid[1], apBssid[2], apBssid[3], apBssid[4], apBssid[5]);
            }
            break;  // Found PMKID, stop searching
        }
    }
}
