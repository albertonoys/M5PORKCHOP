// NetworkRecon - Background WiFi Reconnaissance Service
// Provides shared network scanning for OINK, DONOHAM, and SPECTRUM modes
// Stabilizes heap at boot by running promiscuous mode early
#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include <vector>

// Maximum clients to track per network (shared with modes)
#define MAX_CLIENTS_PER_NETWORK 20

// Maximum networks to track
#define MAX_RECON_NETWORKS 200

// Channel hop order (2.4GHz - most common first)
#define RECON_CHANNEL_COUNT 13

// Heap stabilization typically happens within this time
#define HEAP_STABILIZE_TIMEOUT_MS 500

// Forward declarations - full structs defined in oink.h for now
// TODO: Move these to a shared types header after full migration
struct DetectedClient;
struct DetectedNetwork;

namespace NetworkRecon {

// ============================================================================
// Lifecycle Management
// ============================================================================

/**
 * @brief Initialize the recon service (call once at boot)
 * Sets up mutexes, reserves vector capacity
 */
void init();

/**
 * @brief Start background WiFi promiscuous scanning
 * Enables WiFi, sets up callback, starts channel hopping
 * Heap stabilizes ~115ms after first packets received
 */
void start();

/**
 * @brief Full stop - disables WiFi promiscuous mode
 * Used when entering BLE modes (PIGGYBLUES) that need WiFi OFF
 */
void stop();

/**
 * @brief Pause promiscuous mode but keep WiFi STA active
 * Used when entering ESP-NOW modes (PIGSYNC) that conflict with promiscuous
 */
void pause();

/**
 * @brief Resume promiscuous mode after pause
 * Restores scanning after ESP-NOW mode exits
 */
void resume();

/**
 * @brief Called every loop iteration
 * Handles channel hopping, stale network cleanup, deferred event processing
 */
void update();

// ============================================================================
// State Queries
// ============================================================================

/**
 * @brief Check if recon is actively scanning
 */
bool isRunning();

/**
 * @brief Check if recon is paused (WiFi on but promiscuous off)
 */
bool isPaused();

/**
 * @brief Check if heap has stabilized after start
 * Returns true once largest free block exceeds threshold
 */
bool isHeapStable();

/**
 * @brief Get current scanning channel
 */
uint8_t getCurrentChannel();
uint32_t getHopIntervalMs();

/**
 * @brief Get packet count since start
 */
uint32_t getPacketCount();

// ============================================================================
// Shared Network Data Access
// ============================================================================

/**
 * @brief Get reference to shared networks vector
 * Thread-safe access via internal mutex
 * @warning Do not hold reference across yield() calls
 */
std::vector<DetectedNetwork>& getNetworks();

/**
 * @brief Get network count
 */
uint16_t getNetworkCount();

/**
 * @brief Find network by BSSID and copy to output buffer
 * @param bssid The BSSID to search for
 * @param out Output buffer for network copy (can be nullptr to just check existence)
 * @return true if found, false if not found
 * @note Copies data while holding lock - safe for caller to use after return
 */
bool findNetwork(const uint8_t* bssid, DetectedNetwork* out);

/**
 * @brief Find network index by BSSID
 * @return Index or -1 if not found
 */
int findNetworkIndex(const uint8_t* bssid);

// ============================================================================
// Channel Control
// ============================================================================

/**
 * @brief Lock to specific channel (for targeted operations)
 * Disables channel hopping until unlocked
 */
void lockChannel(uint8_t channel);

/**
 * @brief Unlock channel and resume hopping
 */
void unlockChannel();

/**
 * @brief Check if channel is locked
 */
bool isChannelLocked();

/**
 * @brief Manually set channel (temporary, hopping will override unless locked)
 */
void setChannel(uint8_t channel);

// ============================================================================
// Mode-Specific Callbacks
// ============================================================================

/**
 * @brief Packet callback type for mode-specific processing
 * Called for every received packet after basic network tracking
 * @param pkt The promiscuous packet
 * @param type Packet type (MGMT, CTRL, DATA, MISC)
 */
using PacketCallback = void(*)(const wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type);

/**
 * @brief Register callback for mode-specific packet processing
 * Only one callback active at a time (last registration wins)
 * Pass nullptr to clear callback
 */
void setPacketCallback(PacketCallback callback);

/**
 * @brief New network discovery callback type
 * Called from update() when a new network is added to the shared vector
 * Safe to call Mood/XP functions from this callback (runs in main loop context)
 * @param authmode WiFi auth mode (OPEN, WPA2, WPA3, etc.)
 * @param isHidden True if network has hidden/empty SSID
 * @param ssid Network SSID (may be empty for hidden)
 * @param rssi Signal strength
 * @param channel WiFi channel
 */
using NewNetworkCallback = void(*)(wifi_auth_mode_t authmode, bool isHidden, 
                                   const char* ssid, int8_t rssi, uint8_t channel);

/**
 * @brief Register callback for new network discovery notifications
 * Called from main loop context (safe for XP/Mood calls)
 * Only one callback active at a time (last registration wins)
 * Pass nullptr to clear callback
 */
void setNewNetworkCallback(NewNetworkCallback callback);

// ============================================================================
// Thread Safety
// ============================================================================

/**
 * @brief Enter critical section for network vector access
 * @warning Must call exitCritical() after, keep critical sections short
 */
void enterCritical();

/**
 * @brief Exit critical section
 */
void exitCritical();

/**
 * @brief RAII wrapper for critical section
 */
class CriticalSection {
public:
    CriticalSection() { enterCritical(); }
    ~CriticalSection() { exitCritical(); }
};

} // namespace NetworkRecon
