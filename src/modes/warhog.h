// Warhog Mode - Wardriving with GPS
// Refactored "GPS as Gate" architecture - no entries[] accumulation
#pragma once

#include <Arduino.h>
#include <map>
#include <set>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../gps/gps.h"
#include "../ml/features.h"

// BSSID key for map lookup (6 bytes as uint64_t)
inline uint64_t bssidToKey(const uint8_t* bssid) {
    return ((uint64_t)bssid[0] << 40) | ((uint64_t)bssid[1] << 32) |
           ((uint64_t)bssid[2] << 24) | ((uint64_t)bssid[3] << 16) |
           ((uint64_t)bssid[4] << 8) | bssid[5];
}

class WarhogMode {
public:
    static void init();
    static void start();
    static void stop();
    static void update();
    static bool isRunning() { return running; }
    
    // Scan control
    static void triggerScan();
    static bool isScanComplete();
    
    // Export (data already on disk, these are for format info)
    static bool exportCSV(const char* path);
    static bool exportMLTraining(const char* path);
    
    // GPS
    static bool hasGPSFix();
    static GPSData getGPSData();
    
    // Statistics
    static uint32_t getTotalNetworks() { return totalNetworks; }
    static uint32_t getOpenNetworks() { return openNetworks; }
    static uint32_t getWEPNetworks() { return wepNetworks; }
    static uint32_t getWPANetworks() { return wpaNetworks; }
    static uint32_t getSavedCount() { return savedCount; }  // Geotagged networks (CSV)
    static uint32_t getMLOnlyCount() { return mlOnlyCount; } // ML-only networks (no GPS)
    
private:
    static bool running;
    static uint32_t lastScanTime;
    static uint32_t scanInterval;
    static bool scanInProgress;
    static uint32_t scanStartTime;
    
    static std::set<uint64_t> seenBSSIDs;  // Duplicate tracking for session
    
    // Statistics
    static uint32_t totalNetworks;   // All unique networks seen
    static uint32_t openNetworks;
    static uint32_t wepNetworks;
    static uint32_t wpaNetworks;
    static uint32_t savedCount;      // Networks saved with GPS to CSV
    static uint32_t mlOnlyCount;     // Networks saved to ML file without GPS
    static String currentFilename;   // Current session CSV file
    static String currentMLFilename; // Current session ML training file
    static String currentWigleFilename; // Current session WiGLE CSV file
    
    // Enhanced ML mode - beacon capture
    static bool enhancedMode;
    static std::map<uint64_t, WiFiFeatures> beaconFeatures;
    static uint32_t beaconCount;
    static volatile bool beaconMapBusy;
    
    // Background scan task
    static TaskHandle_t scanTaskHandle;
    static volatile int scanResult;
    
    static void performScan();
    static void scanTask(void* pvParameters);
    static void processScanResults();
    
    // File helpers - write directly per-network
    static bool ensureCSVFileReady();
    static bool ensureMLFileReady();
    static bool ensureWigleFileReady();
    static void appendCSVEntry(const uint8_t* bssid, const char* ssid,
                               int8_t rssi, uint8_t channel, wifi_auth_mode_t auth,
                               double lat, double lon, double alt);
    static void appendMLEntry(const uint8_t* bssid, const char* ssid,
                              const WiFiFeatures& features, uint8_t label,
                              double lat, double lon);
    static void appendWigleEntry(const uint8_t* bssid, const char* ssid,
                                 int8_t rssi, uint8_t channel, wifi_auth_mode_t auth,
                                 double lat, double lon, double alt, double accuracy);
    
    static String authModeToString(wifi_auth_mode_t mode);
    static String authModeToWigleString(wifi_auth_mode_t mode);
    static String generateFilename(const char* ext);
    
    // Enhanced mode promiscuous callback
    static void promiscuousCallback(void* buf, wifi_promiscuous_pkt_type_t type);
    static void startEnhancedCapture();
    static void stopEnhancedCapture();
};
