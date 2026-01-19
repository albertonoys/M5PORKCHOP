// WPA-SEC distributed cracking service client
// https://wpa-sec.stanev.org/
#pragma once

#include <Arduino.h>
#include <map>

// Upload status for tracking
enum class WPASecUploadStatus {
    NOT_UPLOADED,
    UPLOADED,
    CRACKED
};

class WPASec {
public:
    static void init();
    
    // WiFi connection (standalone, uses otaSSID/otaPassword from config)
    static bool connect();
    static void disconnect();
    static bool isConnected();
    
    // API operations (require WiFi connection)
    static bool fetchResults();                      // GET cracked passwords, cache to SD
    static bool uploadCapture(const char* pcapPath); // POST pcap file to WPA-SEC
    static bool isBusy();
    
    // Local cache queries (no WiFi needed)
    static bool loadCache();                         // Load cache from SD
    static bool isCracked(const char* bssid);        // Check if BSSID is cracked
    static String getPassword(const char* bssid);    // Get password for BSSID
    static String getSSID(const char* bssid);        // Get SSID for BSSID (from cache)
    static uint16_t getCrackedCount();               // Total cracked in cache
    
    // Upload tracking
    static bool isUploaded(const char* bssid);       // Check if already uploaded
    /**
     * Mark a BSSID as uploaded.  This function does not reload the
     * entire cache (which would allocate a large amount of heap).  It
     * updates the in-memory uploaded map only if the cache is already
     * loaded, and then appends the entry to the uploaded list file on
     * SD.  This ensures that uploads are remembered across sessions
     * without requiring the full cache to be present in memory.
     */
    static void markUploaded(const char* bssid);
    
    // Status
    static const char* getLastError();
    static const char* getStatus();

    /**
     * @brief Free cached WPA‑SEC results from memory.
     *
     * This releases the internal cracked and uploaded maps to return heap
     * space prior to large TLS operations.  After calling this, the cache
     * will be reloaded from disk on the next lookup or fetch.
     */
    static void freeCacheMemory();
    
private:
    static bool cacheLoaded;
    static char lastError[64];
    static char statusMessage[64];
    static bool busy;
    
    // Cache: BSSID (no colons, uppercase) -> {ssid, password}
    struct CacheEntry {
        String ssid;
        String password;
    };
    static std::map<String, CacheEntry> crackedCache;
    static std::map<String, bool> uploadedCache;
    
    // File paths
    static constexpr const char* CACHE_FILE = "/wpasec_results.txt";
    static constexpr const char* UPLOADED_FILE = "/wpasec_uploaded.txt";
    
    // API endpoints
    static constexpr const char* API_HOST = "wpa-sec.stanev.org";
    static constexpr const char* RESULTS_PATH = "/?api&dl=1";  // Download potfile
    // Uploads are accepted at the root path. Some clients use /?submit,
    // but / is the most compatible and avoids redirect edge cases.
    static constexpr const char* SUBMIT_PATH = "/";
    
    // Helpers
    static String normalizeBSSID(const char* bssid);  // Remove colons, uppercase
    static bool saveCache();
    static bool loadUploadedList();
    static bool saveUploadedList();
};
