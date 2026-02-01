// NetworkRecon - Background WiFi Reconnaissance Service Implementation
// Provides shared network scanning for OINK, DONOHAM, and SPECTRUM modes

#include "network_recon.h"
#include "../modes/oink.h"  // For DetectedNetwork, DetectedClient types
#include "config.h"
#include "wsl_bypasser.h"
#include "wifi_utils.h"
#include "heap_gates.h"
#include "heap_policy.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <NimBLEDevice.h>
#include <atomic>

namespace NetworkRecon {

// ============================================================================
// State Variables
// ============================================================================

static bool initialized = false;
static bool running = false;
static bool paused = false;
static bool channelLocked = false;
static bool channelLockedBeforePause = false;  // [BUG4 FIX] Save state for pause/resume
static uint8_t lockedChannel = 0;
static uint8_t currentChannel = 1;
static uint8_t currentChannelIndex = 0;
static uint32_t lastHopTime = 0;
static uint32_t lastCleanupTime = 0;
static uint32_t startTime = 0;
static volatile uint32_t packetCount = 0;
static std::atomic<bool> busy{false};  // [BUG3 FIX] Atomic for cross-core visibility
static size_t heapLargestAtStart = 0;
static bool heapStabilized = false;

// Channel hop order (most common channels first for faster discovery)
static const uint8_t CHANNEL_HOP_ORDER[RECON_CHANNEL_COUNT] = {
    1, 6, 11, 2, 3, 4, 5, 7, 8, 9, 10, 12, 13
};

// Hop interval in milliseconds
static const uint32_t HOP_INTERVAL_MS = 150;

// Stale network timeout (remove if not seen for this long)
static const uint32_t STALE_TIMEOUT_MS = 60000;

// Cleanup interval
static const uint32_t CLEANUP_INTERVAL_MS = 5000;

// ============================================================================
// Thread Safety
// ============================================================================

static portMUX_TYPE vectorMux = portMUX_INITIALIZER_UNLOCKED;

// ============================================================================
// Shared Data
// ============================================================================

static std::vector<DetectedNetwork> networks;

// ============================================================================
// Deferred Event Processing (avoid allocations in callback)
// ============================================================================

static volatile bool pendingNetworkAdd = false;
static DetectedNetwork pendingNetwork;

// ============================================================================
// Mode-Specific Callbacks
// ============================================================================

static PacketCallback modeCallback = nullptr;
static NewNetworkCallback newNetworkCallback = nullptr;

// ============================================================================
// Internal Functions
// ============================================================================

static void hopChannel() {
    if (channelLocked) return;
    
    currentChannelIndex = (currentChannelIndex + 1) % RECON_CHANNEL_COUNT;
    currentChannel = CHANNEL_HOP_ORDER[currentChannelIndex];
    // #region agent log - H1 channel hop
    {
        static uint32_t lastHopLog = 0;
        uint32_t now = millis();
        if (now - lastHopLog > 1000) {
            lastHopLog = now;
            Serial.printf("[DBG-H1] RECON hop ch=%d locked=%d\n", currentChannel, channelLocked ? 1 : 0);
        }
    }
    // #endregion
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
}

static int findNetworkInternal(const uint8_t* bssid) {
    for (size_t i = 0; i < networks.size(); i++) {
        if (memcmp(networks[i].bssid, bssid, 6) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool detectPMF(const uint8_t* payload, uint16_t len) {
    // Look for RSN IE (0x30) and check RSN Capabilities for MFPC/MFPR bits
    uint16_t offset = 36;
    while (offset + 2 < len) {
        uint8_t id = payload[offset];
        uint8_t ieLen = payload[offset + 1];
        
        if (offset + 2 + ieLen > len) break;
        
        if (id == 0x30 && ieLen >= 8) {  // RSN IE
            // RSN capabilities are at fixed offset from RSN IE start
            // Version (2) + Group Cipher Suite (4) + Pairwise Count (2) + Pairwise Suites (n*4)
            // + AKM Count (2) + AKM Suites (m*4) + RSN Capabilities (2)
            // For simplicity, scan for capabilities at expected position
            uint16_t rsnOffset = offset + 2;
            uint16_t version = payload[rsnOffset] | (payload[rsnOffset + 1] << 8);
            if (version != 1) {
                offset += 2 + ieLen;
                continue;
            }
            
            // Skip to RSN capabilities (simplified - assumes minimal suites)
            // In practice, need to parse suite counts
            uint16_t capOffset = rsnOffset + 8;  // Minimum offset
            if (capOffset + 2 <= offset + 2 + ieLen) {
                uint16_t caps = payload[capOffset] | (payload[capOffset + 1] << 8);
                // MFPC = bit 7, MFPR = bit 6
                if (caps & 0x0080) return true;  // MFPC set
            }
        }
        
        offset += 2 + ieLen;
    }
    return false;
}

static void processBeacon(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    bool hasPMF = detectPMF(payload, len);
    
    // [BUG1 FIX] Lookup under spinlock - vector can be modified by cleanupStaleNetworks()
    taskENTER_CRITICAL(&vectorMux);
    int idx = findNetworkInternal(bssid);
    taskEXIT_CRITICAL(&vectorMux);
    
    if (idx < 0) {
        // New network - queue for deferred add
        if (!pendingNetworkAdd) {
            DetectedNetwork net = {0};
            memcpy(net.bssid, bssid, 6);
            net.rssi = rssi;
            net.lastSeen = millis();
            net.beaconCount = 1;
            net.isTarget = false;
            net.hasPMF = hasPMF;
            net.hasHandshake = false;
            net.attackAttempts = 0;
            net.isHidden = false;
            net.clientCount = 0;
            net.lastClientSeen = 0;
            net.cooldownUntil = 0;
            
            // Parse SSID from IE
            uint16_t offset = 36;
            while (offset + 2 < len) {
                uint8_t id = payload[offset];
                uint8_t ieLen = payload[offset + 1];
                
                if (offset + 2 + ieLen > len) break;
                
                if (id == 0) {
                    if (ieLen > 0 && ieLen <= 32) {
                        memcpy(net.ssid, payload + offset + 2, ieLen);
                        net.ssid[ieLen] = 0;
                        
                        // Check for all-null SSID (hidden)
                        bool allNull = true;
                        for (uint8_t i = 0; i < ieLen; i++) {
                            if (net.ssid[i] != 0) { allNull = false; break; }
                        }
                        if (allNull) {
                            net.isHidden = true;
                        }
                    } else if (ieLen == 0) {
                        net.isHidden = true;
                    }
                    break;
                }
                
                offset += 2 + ieLen;
            }
            
            // Get channel from DS Parameter Set IE
            offset = 36;
            while (offset + 2 < len) {
                uint8_t id = payload[offset];
                uint8_t ieLen = payload[offset + 1];
                
                if (id == 3 && ieLen == 1) {
                    net.channel = payload[offset + 2];
                    break;
                }
                
                offset += 2 + ieLen;
            }
            
            // Parse auth mode
            net.authmode = WIFI_AUTH_OPEN;
            offset = 36;
            while (offset + 2 < len) {
                uint8_t id = payload[offset];
                uint8_t ieLen = payload[offset + 1];
                
                if (offset + 2 + ieLen > len) break;
                
                if (id == 0x30 && ieLen >= 2) {  // RSN IE = WPA2/WPA3
                    if (net.hasPMF) {
                        net.authmode = WIFI_AUTH_WPA3_PSK;
                    } else {
                        net.authmode = WIFI_AUTH_WPA2_PSK;
                    }
                } else if (id == 0xDD && ieLen >= 8) {  // Vendor specific
                    if (payload[offset + 2] == 0x00 && payload[offset + 3] == 0x50 &&
                        payload[offset + 4] == 0xF2 && payload[offset + 5] == 0x01) {
                        if (net.authmode == WIFI_AUTH_OPEN) {
                            net.authmode = WIFI_AUTH_WPA_PSK;
                        } else if (net.authmode == WIFI_AUTH_WPA2_PSK) {
                            net.authmode = WIFI_AUTH_WPA_WPA2_PSK;
                        }
                    }
                }
                
                offset += 2 + ieLen;
            }
            
            if (net.channel == 0) {
                net.channel = currentChannel;
            }
            
            // Queue for deferred add
            memcpy(&pendingNetwork, &net, sizeof(DetectedNetwork));
            pendingNetworkAdd = true;
        }
    } else {
        // Update existing network
        taskENTER_CRITICAL(&vectorMux);
        if (idx >= 0 && idx < (int)networks.size()) {
            networks[idx].rssi = rssi;
            networks[idx].lastSeen = millis();
            networks[idx].beaconCount++;
            networks[idx].hasPMF |= hasPMF;
        }
        taskEXIT_CRITICAL(&vectorMux);
    }
}

static void processProbeResponse(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 36) return;
    
    const uint8_t* bssid = payload + 16;
    
    // [BUG5 FIX] Do lookup inside critical section to prevent TOCTOU race
    // cleanupStaleNetworks() can modify vector between lookup and use
    taskENTER_CRITICAL(&vectorMux);
    int idx = findNetworkInternal(bssid);
    
    if (idx < 0) {
        taskEXIT_CRITICAL(&vectorMux);
        // Check if pending network matches (pendingNetwork is single-slot, no lock needed)
        if (pendingNetworkAdd && memcmp(pendingNetwork.bssid, bssid, 6) == 0) {
            if (pendingNetwork.ssid[0] == 0 || pendingNetwork.isHidden) {
                uint16_t offset = 36;
                while (offset + 2 < len) {
                    uint8_t id = payload[offset];
                    uint8_t ieLen = payload[offset + 1];
                    
                    if (offset + 2 + ieLen > len) break;
                    
                    if (id == 0 && ieLen > 0 && ieLen <= 32) {
                        memcpy(pendingNetwork.ssid, payload + offset + 2, ieLen);
                        pendingNetwork.ssid[ieLen] = 0;
                        
                        bool allNull = true;
                        for (uint8_t i = 0; i < ieLen; i++) {
                            if (pendingNetwork.ssid[i] != 0) { allNull = false; break; }
                        }
                        if (!allNull) {
                            pendingNetwork.isHidden = false;
                            pendingNetwork.lastSeen = millis();
                        }
                        break;
                    }
                    
                    offset += 2 + ieLen;
                }
            }
        }
        return;
    }
    
    // idx is valid and we hold the lock - safe to use
    if (networks[idx].ssid[0] == 0 || networks[idx].isHidden) {
        uint16_t offset = 36;
        while (offset + 2 < len) {
            uint8_t id = payload[offset];
            uint8_t ieLen = payload[offset + 1];
            
            if (offset + 2 + ieLen > len) break;
            
            if (id == 0 && ieLen > 0 && ieLen <= 32) {
                memcpy(networks[idx].ssid, payload + offset + 2, ieLen);
                networks[idx].ssid[ieLen] = 0;
                networks[idx].isHidden = false;
                break;
            }
            
            offset += 2 + ieLen;
        }
    }
    
    networks[idx].lastSeen = millis();
    taskEXIT_CRITICAL(&vectorMux);
}

static void trackClient(const uint8_t* bssid, const uint8_t* clientMac, int8_t rssi) {
    taskENTER_CRITICAL(&vectorMux);
    
    int idx = findNetworkInternal(bssid);
    if (idx < 0 || idx >= (int)networks.size()) {
        taskEXIT_CRITICAL(&vectorMux);
        return;
    }
    
    DetectedNetwork& net = networks[idx];
    
    // Check if client already tracked
    for (uint8_t i = 0; i < net.clientCount; i++) {
        if (memcmp(net.clients[i].mac, clientMac, 6) == 0) {
            net.clients[i].rssi = rssi;
            net.clients[i].lastSeen = millis();
            net.lastClientSeen = millis();
            taskEXIT_CRITICAL(&vectorMux);
            return;
        }
    }
    
    // Add new client if space available
    if (net.clientCount < MAX_CLIENTS_PER_NETWORK) {
        memcpy(net.clients[net.clientCount].mac, clientMac, 6);
        net.clients[net.clientCount].rssi = rssi;
        net.clients[net.clientCount].lastSeen = millis();
        net.clientCount++;
        net.lastClientSeen = millis();
    }
    
    taskEXIT_CRITICAL(&vectorMux);
}

static void processDataFrame(const uint8_t* payload, uint16_t len, int8_t rssi) {
    if (len < 28) return;
    
    uint8_t toDs = (payload[1] & 0x01);
    uint8_t fromDs = (payload[1] & 0x02) >> 1;
    
    const uint8_t* bssid = nullptr;
    const uint8_t* clientMac = nullptr;
    
    if (!toDs && fromDs) {
        bssid = payload + 10;
        clientMac = payload + 4;
    } else if (toDs && !fromDs) {
        bssid = payload + 4;
        clientMac = payload + 10;
    }
    
    if (bssid && clientMac) {
        if ((clientMac[0] & 0x01) == 0) {
            trackClient(bssid, clientMac, rssi);
        }
    }
}

static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf) return;
    if (!running || paused) return;
    if (busy) return;
    
    wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    int8_t rssi = pkt->rx_ctrl.rssi;
    
    // ESP32 adds 4 ghost bytes
    if (len > 4) len -= 4;
    if (len < 24) return;
    
    packetCount++;
    
    const uint8_t* payload = pkt->payload;
    uint8_t frameSubtype = (payload[0] >> 4) & 0x0F;
    
    // Basic network tracking (always happens)
    switch (type) {
        case WIFI_PKT_MGMT:
            if (frameSubtype == 0x08) {  // Beacon
                processBeacon(payload, len, rssi);
            } else if (frameSubtype == 0x05) {  // Probe Response
                processProbeResponse(payload, len, rssi);
            }
            break;
            
        case WIFI_PKT_DATA:
            processDataFrame(payload, len, rssi);
            break;
            
        default:
            break;
    }
    
    // Mode-specific callback (for EAPOL capture, PCAP logging, etc.)
    if (modeCallback) {
        modeCallback(pkt, type);
    }
}

static void processDeferredEvents() {
    // [BUG2 FIX] Set busy BEFORE checking flag - prevents callback overwrite race
    busy = true;
    
    if (pendingNetworkAdd) {
        
        // Check capacity OUTSIDE critical section
        bool shouldAdd = false;
        size_t capacityLimit = networks.capacity();
        bool hasCapacity = networks.size() < capacityLimit;
        bool canGrow = networks.size() < MAX_RECON_NETWORKS;
        
        // Only add if we have pre-reserved capacity (no allocation needed)
        if (hasCapacity) {
            shouldAdd = true;
        } else if (canGrow &&
                   HeapGates::canGrow(HeapPolicy::kMinHeapForReconGrowth,
                                      HeapPolicy::kMinFragRatioForGrowth)) {
            // Grow capacity OUTSIDE critical section to avoid heap ops while holding spinlock
            try {
                networks.reserve(networks.capacity() + 20);
                shouldAdd = true;
            } catch (...) {
                // OOM - skip this network
            }
        }
        
        if (shouldAdd) {
            taskENTER_CRITICAL(&vectorMux);
            networks.push_back(pendingNetwork);  // Safe: capacity pre-reserved
            taskEXIT_CRITICAL(&vectorMux);
            
            // Notify mode of new network discovery (for XP events)
            // Called OUTSIDE critical section - safe for Mood/XP calls
            if (newNetworkCallback) {
                newNetworkCallback(
                    pendingNetwork.authmode,
                    pendingNetwork.isHidden,
                    pendingNetwork.ssid,
                    pendingNetwork.rssi,
                    pendingNetwork.channel
                );
            }
        }
        
        pendingNetworkAdd = false;
    }
    
    busy = false;
}

static void cleanupStaleNetworks() {
    uint32_t now = millis();
    
    // [BUG6 FIX] Single critical section for collect + erase
    // Previously had gap between collect and erase where vector could change
    // erase() doesn't allocate - just shifts elements and decrements size - safe in spinlock
    taskENTER_CRITICAL(&vectorMux);
    
    // Collect stale indices (static to avoid stack/heap allocation)
    static size_t staleIndices[20];
    size_t staleCount = 0;
    
    for (size_t i = 0; i < networks.size() && staleCount < 20; i++) {
        if (now - networks[i].lastSeen > STALE_TIMEOUT_MS) {
            staleIndices[staleCount++] = i;
        }
    }
    
    // Erase in reverse order to preserve indices
    for (int i = staleCount - 1; i >= 0; i--) {
        // No bounds check needed - indices are valid, no vector modification between collect and erase
        networks.erase(networks.begin() + staleIndices[i]);
    }
    
    taskEXIT_CRITICAL(&vectorMux);
}

// ============================================================================
// Public API Implementation
// ============================================================================

void init() {
    if (initialized) return;
    
    Serial.println("[RECON] Initializing NetworkRecon service...");
    
    networks.clear();
    networks.reserve(50);  // Initial reserve, will grow as needed
    
    packetCount = 0;
    currentChannel = 1;
    currentChannelIndex = 0;
    lastHopTime = 0;
    lastCleanupTime = 0;
    running = false;
    paused = false;
    channelLocked = false;
    busy = false;
    pendingNetworkAdd = false;
    modeCallback = nullptr;
    heapStabilized = false;
    
    initialized = true;
    Serial.println("[RECON] Initialized");
}

void start() {
    if (!initialized) init();
    if (running) {
        if (paused) {
            resume();
        }
        return;
    }
    
    Serial.printf("[RECON] Starting background scan... free=%u largest=%u\n",
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    
    heapLargestAtStart = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    heapStabilized = false;
    startTime = millis();
    
    // Handle BLE coexistence
    if (NimBLEDevice::isInitialized()) {
        Serial.println("[RECON] BLE active - deinitializing for WiFi coexistence");
        
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (pScan && pScan->isScanning()) {
            pScan->stop();
            delay(50);
        }
        
        NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
        if (pAdv && pAdv->isAdvertising()) {
            pAdv->stop();
            delay(50);
        }
        
        NimBLEDevice::deinit(true);
        delay(100);
        
        Serial.printf("[RECON] After BLE deinit: free=%u largest=%u\n",
                      ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    
    // Initialize WiFi
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    delay(50);
    
    // Randomize MAC if configured
    if (Config::wifi().randomizeMAC) {
        WSLBypasser::randomizeMAC();
    }
    
    WiFi.disconnect();
    delay(50);
    
    // Set up promiscuous mode
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous_filter(nullptr);  // Receive all packet types
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    running = true;
    paused = false;
    lastHopTime = millis();
    lastCleanupTime = millis();
    
    Serial.printf("[RECON] Started on channel %d\n", currentChannel);
}

void stop() {
    if (!running) return;
    
    Serial.println("[RECON] Stopping...");
    
    running = false;
    paused = false;
    
    WiFiUtils::stopPromiscuous();
    
    // Don't clear networks - they persist for mode reuse
    
    Serial.printf("[RECON] Stopped. Networks cached: %d\n", networks.size());
}

void pause() {
    if (!running || paused) return;
    
    Serial.println("[RECON] Pausing promiscuous mode...");
    
    paused = true;
    
    // [BUG4 FIX] Save and clear channel lock - will restore on resume if mode still active
    channelLockedBeforePause = channelLocked;
    if (channelLocked) {
        channelLocked = false;
        Serial.println("[RECON] Channel lock suspended for pause");
    }
    
    // Disable promiscuous but keep WiFi STA active
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    
    Serial.println("[RECON] Paused (WiFi STA still active)");
}

void resume() {
    if (!running || !paused) return;
    
    Serial.println("[RECON] Resuming promiscuous mode...");
    
    // Disconnect from any network before enabling promiscuous mode
    // (WiFi may be connected after TLS operations like WiGLE/WPA-SEC sync)
    WiFi.disconnect();
    delay(50);
    
    // Re-enable promiscuous
    esp_wifi_set_promiscuous_rx_cb(promiscuousCallback);
    esp_wifi_set_promiscuous_filter(nullptr);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    
    paused = false;
    lastHopTime = millis();
    
    // [BUG4 FIX] Restore channel lock only if mode callback still registered
    // (If modeCallback is null, no mode owns the lock anymore)
    if (channelLockedBeforePause && modeCallback != nullptr) {
        channelLocked = true;
        Serial.printf("[RECON] Channel lock restored to %d\n", lockedChannel);
    }
    channelLockedBeforePause = false;
    
    Serial.printf("[RECON] Resumed on channel %d\n", currentChannel);
}

void update() {
    if (!running || paused) return;
    
    uint32_t now = millis();
    
    // Process deferred events from callback
    processDeferredEvents();
    
    // Channel hopping
    if (!channelLocked && now - lastHopTime > HOP_INTERVAL_MS) {
        hopChannel();
        lastHopTime = now;
    }
    
    // Periodic cleanup
    if (now - lastCleanupTime > CLEANUP_INTERVAL_MS) {
        cleanupStaleNetworks();
        lastCleanupTime = now;
    }
    
    // Check heap stabilization
    if (!heapStabilized) {
        size_t currentLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (currentLargest > HeapPolicy::kHeapStableThreshold) {
            heapStabilized = true;
            Serial.printf("[RECON] Heap stabilized in %ums: largest=%u (was %u)\n",
                          now - startTime, currentLargest, heapLargestAtStart);
        }
    }
}

bool isRunning() {
    return running && !paused;
}

bool isPaused() {
    return running && paused;
}

bool isHeapStable() {
    return heapStabilized;
}

uint8_t getCurrentChannel() {
    return currentChannel;
}

uint32_t getHopIntervalMs() {
    return HOP_INTERVAL_MS;
}

uint32_t getPacketCount() {
    return packetCount;
}

std::vector<DetectedNetwork>& getNetworks() {
    return networks;
}

uint16_t getNetworkCount() {
    return networks.size();
}

bool findNetwork(const uint8_t* bssid, DetectedNetwork* out) {
    taskENTER_CRITICAL(&vectorMux);
    int idx = findNetworkInternal(bssid);
    bool found = (idx >= 0 && idx < (int)networks.size());
    if (found && out) {
        // Copy to caller's buffer while holding lock - pointer becomes invalid after unlock
        *out = networks[idx];
    }
    taskEXIT_CRITICAL(&vectorMux);
    return found;
}

int findNetworkIndex(const uint8_t* bssid) {
    taskENTER_CRITICAL(&vectorMux);
    int idx = findNetworkInternal(bssid);
    taskEXIT_CRITICAL(&vectorMux);
    return idx;
}

void lockChannel(uint8_t channel) {
    if (channel < 1 || channel > 14) return;
    
    channelLocked = true;
    lockedChannel = channel;
    currentChannel = channel;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    Serial.printf("[RECON] Channel locked to %d\n", channel);
}

void unlockChannel() {
    channelLocked = false;
    Serial.println("[RECON] Channel unlocked, resuming hopping");
}

bool isChannelLocked() {
    return channelLocked;
}

void setChannel(uint8_t channel) {
    if (channel < 1 || channel > 14) return;
    currentChannel = channel;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

void setPacketCallback(PacketCallback callback) {
    modeCallback = callback;
}

void setNewNetworkCallback(NewNetworkCallback callback) {
    newNetworkCallback = callback;
}

void enterCritical() {
    taskENTER_CRITICAL(&vectorMux);
}

void exitCritical() {
    taskEXIT_CRITICAL(&vectorMux);
}

} // namespace NetworkRecon
