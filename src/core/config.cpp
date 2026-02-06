// src/core/config.cpp
// Configuration management implementation

#include "config.h"
#include "sdlog.h"
#include "sd_layout.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <SPIFFS.h>
#include <SPI.h>
#include <driver/gpio.h>

// ---- Cardputer microSD wiring (explicit, per Cardputer v1.1 schematic) ----
// ESP32-S3FN8:
//   microSD Socket  CS   MOSI  CLK   MISO
//                  G12  G14   G40   G39
//
// (Your previous patch used ESP32 “classic” pins + CS=4, which breaks SD on Cardputer/StampS3.)
static constexpr int SD_CS_PIN   = 12;  // CS
static constexpr int SD_MOSI_PIN = 14;  // MOSI
static constexpr int SD_MISO_PIN = 39;  // MISO
static constexpr int SD_SCK_PIN  = 40;  // SCK/CLK

// Dedicated SPI bus instance for SD.
// Cardputer microSD pinmap (from M5 docs): CS=12 MOSI=14 CLK=40 MISO=39.
// In practice, Arduino-ESP32/PlatformIO combos vary; using FSPI with explicit
// pins is the most reliable on Cardputer builds.
static SPIClass sdSPI(FSPI);
static bool sdSpiBegun = false;

// Static member initialization
GPSConfig Config::gpsConfig;
MLConfig Config::mlConfig;
WiFiConfig Config::wifiConfig;
BLEConfig Config::bleConfig;
PersonalityConfig Config::personalityConfig;
bool Config::initialized = false;
static bool sdAvailable = false;

static uint16_t clampU16(uint32_t value, uint16_t minVal, uint16_t maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return static_cast<uint16_t>(value);
}

static int8_t clampI8(int value, int8_t minVal, int8_t maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return static_cast<int8_t>(value);
}

static void sanitizeWiFiConfig(WiFiConfig& cfg) {
    cfg.channelHopInterval = clampU16(cfg.channelHopInterval, 50, 2000);
    cfg.spectrumHopInterval = clampU16(cfg.spectrumHopInterval, 50, 2000);
    cfg.spectrumMinRssi = clampI8(cfg.spectrumMinRssi, -95, -30);
    cfg.attackMinRssi = clampI8(cfg.attackMinRssi, -90, -50);
    if (cfg.spectrumTopN > 100) cfg.spectrumTopN = 100;
    cfg.spectrumStaleMs = clampU16(cfg.spectrumStaleMs, 1000, 20000);
}

static void ensureSdSpiReady() {
    // Re-init SD SPI bus cleanly
    if (sdSpiBegun) {
        sdSPI.end();
        sdSpiBegun = false;
        delay(20);
    }

    // Make sure CS is a sane GPIO output and deasserted before touching the bus.
    // This prevents random Select Failed errors on some cards.
    pinMode(SD_CS_PIN, OUTPUT);
    digitalWrite(SD_CS_PIN, HIGH);

    // SCK, MISO, MOSI, SS/CS
    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    sdSpiBegun = true;
    delay(20);
}

bool Config::init() {
    // Initialize SPIFFS first (always available)
    if (!SPIFFS.begin(true)) {
        Serial.println("[CONFIG] SPIFFS mount failed");
    }

    // Allow buses to stabilize after M5.begin()
    delay(50);

    // Ensure SD has a proper SPI bus configured
    ensureSdSpiReady();

    // Retry with progressive SPI speeds for reliability
    sdAvailable = false;
    const int maxRetries = 6;
    const uint32_t speeds[] = {
        25000000, // 25 MHz
        20000000, // 20 MHz
        10000000, // 10 MHz
        8000000,  // 8 MHz
        4000000,  // 4 MHz
        1000000   // 1 MHz
    };

    for (int attempt = 0; attempt < maxRetries && !sdAvailable; attempt++) {
        uint32_t speed = speeds[attempt];
        Serial.printf("[CONFIG] SD init attempt %d/%d at %luMHz\n",
                      attempt + 1, maxRetries, speed / 1000000);

        if (attempt > 0) {
            SD.end();     // Clean up previous failed attempt
            delay(80);    // Allow bus to settle
            ensureSdSpiReady();
        }

        // Use explicit CS + dedicated SPI + explicit speed
        if (SD.begin(SD_CS_PIN, sdSPI, speed)) {
            Serial.printf("[CONFIG] SD card mounted at %luMHz\n", speed / 1000000);
            sdAvailable = true;
        }
    }

    if (!sdAvailable) {
        SDLayout::setUseNewLayout(false);
        Serial.println("[CONFIG] SD card init failed after retries, using SPIFFS");
    } else {
        SDLayout::migrateIfNeeded();
        SDLayout::ensureDirs();
        SDLog::log("CFG", "SD card mounted OK");
    }

    // Load personality from SPIFFS (always available)
    if (!loadPersonality()) {
        Serial.println("[CONFIG] Creating default personality");
        createDefaultPersonality();
        savePersonalityToSPIFFS();
    }

    // Load main config (SD if available, otherwise SPIFFS)
    if (!load()) {
        Serial.println("[CONFIG] Creating default config");
        createDefaultConfig();
        save();  // persist defaults to whichever FS is active
    }

    // Try to load WPA-SEC key from file (auto-deletes after import)
    if (loadWpaSecKeyFromFile()) {
        Serial.println("[CONFIG] WPA-SEC key loaded from file");
    }

    initialized = true;
    return true;
}

bool Config::isSDAvailable() {
    return sdAvailable;
}

void Config::prepareSDBus() {
    ensureSdSpiReady();
}

SPIClass& Config::sdSpi() {
    return sdSPI;
}

int Config::sdCsPin() {
    return SD_CS_PIN;
}

void Config::prepareCapLoraGpio() {
    // GPIO 13 is ESP32-S3 default FSPIQ (MISO) via IOMUX. Even though SD remaps
    // FSPI MISO to G39, the default IOMUX linkage on G13 can disrupt the FSPI
    // peripheral when Serial2 reconfigures G13 as UART TX output.
    // gpio_reset_pin() clears IOMUX function, disconnects peripheral signals,
    // and returns the pin to plain GPIO mode.
    gpio_reset_pin(static_cast<gpio_num_t>(CapLoraPins::GPS_TX));   // G13

    // Reset SX1262 LoRa chip to known state. The CapLoRa868 LoRa SPI shares
    // MOSI(G14)/MISO(G39)/SCK(G40) with SD card. After reset the SX1262
    // enters STANDBY_RC with all IOs high-impedance, preventing bus contention.
    pinMode(CapLoraPins::LORA_RESET, OUTPUT);
    digitalWrite(CapLoraPins::LORA_RESET, LOW);   // Assert NRESET (active low)
    delay(10);                                      // SX1262 datasheet: >100us
    digitalWrite(CapLoraPins::LORA_RESET, HIGH);   // Release reset
    delay(10);                                      // Wait for standby entry

    // Deassert LoRa chip select (HIGH = not selected, MISO tri-stated)
    pinMode(CapLoraPins::LORA_CS, OUTPUT);
    digitalWrite(CapLoraPins::LORA_CS, HIGH);

    // Configure control pins as inputs (don't drive)
    pinMode(CapLoraPins::LORA_BUSY, INPUT);
    pinMode(CapLoraPins::LORA_DIO1, INPUT);

    Serial.println("[CONFIG] CapLoRa868: SX1262 reset, CS deasserted, G13 IOMUX cleared");
}

bool Config::reinitSD() {
    Serial.println("[CONFIG] Attempting SD card re-initialization...");

    // Clean up any existing SD state
    SD.end();
    delay(80);

    // Re-init SD SPI bus explicitly
    ensureSdSpiReady();

    // Retry with progressive SPI speeds
    sdAvailable = false;
    const int maxRetries = 6;
    const uint32_t speeds[] = {
        25000000,
        20000000,
        10000000,
        8000000,
        4000000,
        1000000
    };

    for (int attempt = 0; attempt < maxRetries && !sdAvailable; attempt++) {
        uint32_t speed = speeds[attempt];
        Serial.printf("[CONFIG] SD reinit attempt %d/%d at %luMHz\n",
                      attempt + 1, maxRetries, speed / 1000000);

        if (attempt > 0) {
            SD.end();
            delay(80);
            ensureSdSpiReady();
        }

        if (SD.begin(SD_CS_PIN, sdSPI, speed)) {
            Serial.printf("[CONFIG] SD card mounted at %luMHz\n", speed / 1000000);
            sdAvailable = true;
        }
    }

    if (sdAvailable) {
        SDLayout::migrateIfNeeded();
        SDLayout::ensureDirs();
        SDLog::log("CFG", "SD card re-initialized OK");
    } else {
        SDLayout::setUseNewLayout(false);
        Serial.println("[CONFIG] SD card reinit failed");
    }

    return sdAvailable;
}

bool Config::load() {
    // Use SD if available, else SPIFFS fallback
    fs::FS& cfgFS = sdAvailable ? (fs::FS&)SD : (fs::FS&)SPIFFS;

    const char* cfgPath = sdAvailable ? SDLayout::configPathSD() : CONFIG_FILE;
    File file = cfgFS.open(cfgPath, FILE_READ);
    if (!file && sdAvailable && SDLayout::usingNewLayout()) {
        file = cfgFS.open(SDLayout::legacyConfigPath(), FILE_READ);
    }
    if (!file) {
        Serial.println("[CONFIG] Cannot open config file");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.printf("[CONFIG] JSON parse error: %s\n", err.c_str());
        return false;
    }

    // GPS config
    if (doc["gps"].is<JsonObject>()) {
        gpsConfig.enabled = doc["gps"]["enabled"] | true;
        gpsConfig.source = static_cast<GPSSource>(doc["gps"]["gpsSource"] | 0);

        // Auto-set pins based on source, or load custom pins
        if (gpsConfig.source == GPSSource::CAP_LORA) {
            gpsConfig.rxPin = 15;  // Cap LoRa868 GPS RX
            gpsConfig.txPin = 13;  // Cap LoRa868 GPS TX
        } else if (gpsConfig.source == GPSSource::GROVE) {
            gpsConfig.rxPin = 1;   // Grove GPS RX
            gpsConfig.txPin = 2;   // Grove GPS TX
        } else {
            // CUSTOM: load pins from config
            gpsConfig.rxPin = doc["gps"]["rxPin"] | 1;
            gpsConfig.txPin = doc["gps"]["txPin"] | 2;
        }

        gpsConfig.baudRate = doc["gps"]["baudRate"] | 115200;
        gpsConfig.updateInterval = doc["gps"]["updateInterval"] | 5;
        gpsConfig.sleepTimeMs = doc["gps"]["sleepTimeMs"] | 5000;
        gpsConfig.powerSave = doc["gps"]["powerSave"] | true;
        gpsConfig.timezoneOffset = doc["gps"]["timezoneOffset"] | 0;
    }

    // ML config
    if (doc["ml"].is<JsonObject>()) {
        mlConfig.enabled = doc["ml"]["enabled"] | true;
        mlConfig.collectionMode = static_cast<MLCollectionMode>(doc["ml"]["collectionMode"] | 0);
        mlConfig.modelPath = doc["ml"]["modelPath"] | "/m5porkchop/models/porkchop_model.bin";
        if (sdAvailable && SDLayout::usingNewLayout()) {
            if (mlConfig.modelPath.startsWith("/models/")) {
                mlConfig.modelPath = String(SDLayout::modelsDir()) + mlConfig.modelPath.substring(7);
            }
        }
        mlConfig.confidenceThreshold = doc["ml"]["confidenceThreshold"] | 0.7f;
        mlConfig.rogueApThreshold = doc["ml"]["rogueApThreshold"] | 0.8f;
        mlConfig.vulnScorerThreshold = doc["ml"]["vulnScorerThreshold"] | 0.6f;
        mlConfig.autoUpdate = doc["ml"]["autoUpdate"] | false;
        mlConfig.updateUrl = doc["ml"]["updateUrl"] | "";
    }

    // WiFi config
    if (doc["wifi"].is<JsonObject>()) {
        int hopInterval = doc["wifi"]["channelHopInterval"] | 150;
        wifiConfig.channelHopInterval = clampU16(hopInterval, 50, 2000);
        int spectrumHop = doc["wifi"]["spectrumHopInterval"] | 150;
        wifiConfig.spectrumHopInterval = clampU16(spectrumHop, 50, 2000);
        wifiConfig.lockTime = doc["wifi"]["lockTime"] | 12000;
        wifiConfig.enableDeauth = doc["wifi"]["enableDeauth"] | true;
        wifiConfig.randomizeMAC = doc["wifi"]["randomizeMAC"] | true;
        int attackRssi = doc["wifi"]["attackMinRssi"] | -70;
        wifiConfig.attackMinRssi = clampI8(attackRssi, -90, -50);
        int minRssi = doc["wifi"]["spectrumMinRssi"] | -95;
        wifiConfig.spectrumMinRssi = clampI8(minRssi, -95, -30);
        int topN = doc["wifi"]["spectrumTopN"] | 0;
        if (topN < 0) topN = 0;
        if (topN > 100) topN = 100;
        wifiConfig.spectrumTopN = static_cast<uint8_t>(topN);
        int staleMs = doc["wifi"]["spectrumStaleMs"] | 5000;
        wifiConfig.spectrumStaleMs = clampU16(staleMs, 1000, 20000);
        wifiConfig.spectrumCollapseSsid = doc["wifi"]["spectrumCollapseSsid"] | false;
        wifiConfig.spectrumTiltEnabled = doc["wifi"]["spectrumTiltEnabled"] | true;
        const char* ssid = doc["wifi"]["otaSSID"] | "";
        strncpy(wifiConfig.otaSSID, ssid, sizeof(wifiConfig.otaSSID) - 1);
        wifiConfig.otaSSID[sizeof(wifiConfig.otaSSID) - 1] = '\0';
        const char* password = doc["wifi"]["otaPassword"] | "";
        strncpy(wifiConfig.otaPassword, password, sizeof(wifiConfig.otaPassword) - 1);
        wifiConfig.otaPassword[sizeof(wifiConfig.otaPassword) - 1] = '\0';
        wifiConfig.autoConnect = doc["wifi"]["autoConnect"] | false;
        const char* key = doc["wifi"]["wpaSecKey"] | "";
        strncpy(wifiConfig.wpaSecKey, key, sizeof(wifiConfig.wpaSecKey) - 1);
        wifiConfig.wpaSecKey[sizeof(wifiConfig.wpaSecKey) - 1] = '\0';
        const char* apiName = doc["wifi"]["wigleApiName"] | "";
        strncpy(wifiConfig.wigleApiName, apiName, sizeof(wifiConfig.wigleApiName) - 1);
        wifiConfig.wigleApiName[sizeof(wifiConfig.wigleApiName) - 1] = '\0';
        const char* apiToken = doc["wifi"]["wigleApiToken"] | "";
        strncpy(wifiConfig.wigleApiToken, apiToken, sizeof(wifiConfig.wigleApiToken) - 1);
        wifiConfig.wigleApiToken[sizeof(wifiConfig.wigleApiToken) - 1] = '\0';
    }
    sanitizeWiFiConfig(wifiConfig);

    // BLE config (PIGGY BLUES)
    if (doc["ble"].is<JsonObject>()) {
        bleConfig.burstInterval = doc["ble"]["burstInterval"] | 200;
        bleConfig.advDuration = doc["ble"]["advDuration"] | 100;
    }

    Serial.println("[CONFIG] Loaded successfully");
    return true;
}

bool Config::loadPersonality() {
    // Load from SPIFFS (always available)
    File file = SPIFFS.open(PERSONALITY_FILE, FILE_READ);
    if (!file) {
        Serial.println("[CONFIG] Personality file not found in SPIFFS");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.printf("[CONFIG] Personality JSON error: %s\n", err.c_str());
        return false;
    }

    const char* name = doc["name"] | "Porkchop";
    strncpy(personalityConfig.name, name, sizeof(personalityConfig.name) - 1);
    personalityConfig.name[sizeof(personalityConfig.name) - 1] = '\0';

    const char* callsign = doc["callsign"] | "";
    strncpy(personalityConfig.callsign, callsign, sizeof(personalityConfig.callsign) - 1);
    personalityConfig.callsign[sizeof(personalityConfig.callsign) - 1] = '\0';

    personalityConfig.mood = doc["mood"] | 50;
    personalityConfig.experience = doc["experience"] | 0;
    personalityConfig.curiosity = doc["curiosity"] | 0.7f;
    personalityConfig.aggression = doc["aggression"] | 0.3f;
    personalityConfig.patience = doc["patience"] | 0.5f;
    personalityConfig.soundEnabled = doc["soundEnabled"] | true;
    personalityConfig.brightness = doc["brightness"] | 80;
    personalityConfig.dimLevel = doc["dimLevel"] | 20;
    personalityConfig.dimTimeout = doc["dimTimeout"] | 30;
    personalityConfig.themeIndex = doc["themeIndex"] | 0;
    uint8_t g0Action = doc["g0Action"] | static_cast<uint8_t>(G0Action::SCREEN_TOGGLE);
    if (g0Action >= G0_ACTION_COUNT) {
        g0Action = static_cast<uint8_t>(G0Action::SCREEN_TOGGLE);
    }
    personalityConfig.g0Action = static_cast<G0Action>(g0Action);
    uint8_t bootMode = doc["bootMode"] | static_cast<uint8_t>(BootMode::IDLE);
    if (bootMode >= BOOT_MODE_COUNT) {
        bootMode = static_cast<uint8_t>(BootMode::IDLE);
    }
    personalityConfig.bootMode = static_cast<BootMode>(bootMode);

    Serial.printf("[CONFIG] Personality: %s (mood: %d, sound: %s, bright: %d%%, dim: %ds, theme: %d)\n",
                  personalityConfig.name,
                  personalityConfig.mood,
                  personalityConfig.soundEnabled ? "ON" : "OFF",
                  personalityConfig.brightness,
                  personalityConfig.dimTimeout,
                  personalityConfig.themeIndex);
    return true;
}

void Config::savePersonalityToSPIFFS() {
    JsonDocument doc;
    doc["name"] = personalityConfig.name;
    doc["callsign"] = personalityConfig.callsign;
    doc["mood"] = personalityConfig.mood;
    doc["experience"] = personalityConfig.experience;
    doc["curiosity"] = personalityConfig.curiosity;
    doc["aggression"] = personalityConfig.aggression;
    doc["patience"] = personalityConfig.patience;
    doc["soundEnabled"] = personalityConfig.soundEnabled;
    doc["brightness"] = personalityConfig.brightness;
    doc["dimLevel"] = personalityConfig.dimLevel;
    doc["dimTimeout"] = personalityConfig.dimTimeout;
    doc["themeIndex"] = personalityConfig.themeIndex;
    doc["g0Action"] = static_cast<uint8_t>(personalityConfig.g0Action);
    doc["bootMode"] = static_cast<uint8_t>(personalityConfig.bootMode);

    File file = SPIFFS.open(PERSONALITY_FILE, FILE_WRITE);
    if (file) {
        serializeJsonPretty(doc, file);
        file.close();
        Serial.printf("[CONFIG] Saved personality to SPIFFS (sound: %s)\n",
                      personalityConfig.soundEnabled ? "ON" : "OFF");
    } else {
        Serial.println("[CONFIG] Failed to save personality to SPIFFS");
    }
}

bool Config::save() {
    JsonDocument doc;

    // GPS config
    doc["gps"]["enabled"] = gpsConfig.enabled;
    doc["gps"]["gpsSource"] = static_cast<uint8_t>(gpsConfig.source);
    doc["gps"]["rxPin"] = gpsConfig.rxPin;
    doc["gps"]["txPin"] = gpsConfig.txPin;
    doc["gps"]["baudRate"] = gpsConfig.baudRate;
    doc["gps"]["updateInterval"] = gpsConfig.updateInterval;
    doc["gps"]["sleepTimeMs"] = gpsConfig.sleepTimeMs;
    doc["gps"]["powerSave"] = gpsConfig.powerSave;
    doc["gps"]["timezoneOffset"] = gpsConfig.timezoneOffset;

    // ML config
    doc["ml"]["enabled"] = mlConfig.enabled;
    doc["ml"]["collectionMode"] = static_cast<uint8_t>(mlConfig.collectionMode);
    doc["ml"]["modelPath"] = mlConfig.modelPath;
    doc["ml"]["confidenceThreshold"] = mlConfig.confidenceThreshold;
    doc["ml"]["rogueApThreshold"] = mlConfig.rogueApThreshold;
    doc["ml"]["vulnScorerThreshold"] = mlConfig.vulnScorerThreshold;
    doc["ml"]["autoUpdate"] = mlConfig.autoUpdate;
    doc["ml"]["updateUrl"] = mlConfig.updateUrl;

    // WiFi config
    doc["wifi"]["channelHopInterval"] = wifiConfig.channelHopInterval;
    doc["wifi"]["spectrumHopInterval"] = wifiConfig.spectrumHopInterval;
    doc["wifi"]["lockTime"] = wifiConfig.lockTime;
    doc["wifi"]["enableDeauth"] = wifiConfig.enableDeauth;
    doc["wifi"]["randomizeMAC"] = wifiConfig.randomizeMAC;
    doc["wifi"]["attackMinRssi"] = wifiConfig.attackMinRssi;
    doc["wifi"]["spectrumMinRssi"] = wifiConfig.spectrumMinRssi;
    doc["wifi"]["spectrumTopN"] = wifiConfig.spectrumTopN;
    doc["wifi"]["spectrumStaleMs"] = wifiConfig.spectrumStaleMs;
    doc["wifi"]["spectrumCollapseSsid"] = wifiConfig.spectrumCollapseSsid;
    doc["wifi"]["spectrumTiltEnabled"] = wifiConfig.spectrumTiltEnabled;
    doc["wifi"]["otaSSID"] = wifiConfig.otaSSID;
    doc["wifi"]["otaPassword"] = wifiConfig.otaPassword;
    doc["wifi"]["autoConnect"] = wifiConfig.autoConnect;
    doc["wifi"]["wpaSecKey"] = wifiConfig.wpaSecKey;
    doc["wifi"]["wigleApiName"] = wifiConfig.wigleApiName;
    doc["wifi"]["wigleApiToken"] = wifiConfig.wigleApiToken;

    // BLE config
    doc["ble"]["burstInterval"] = bleConfig.burstInterval;
    doc["ble"]["advDuration"] = bleConfig.advDuration;

    // Save to SD if available, otherwise SPIFFS
    fs::FS& cfgFS = sdAvailable ? (fs::FS&)SD : (fs::FS&)SPIFFS;

    const char* cfgPath = sdAvailable ? SDLayout::configPathSD() : CONFIG_FILE;
    File file = cfgFS.open(cfgPath, FILE_WRITE);
    if (!file) {
        return false;
    }

    size_t written = serializeJsonPretty(doc, file);
    file.close();

    return written > 0;
}

bool Config::createDefaultConfig() {
    gpsConfig = GPSConfig();
    mlConfig = MLConfig();
    wifiConfig = WiFiConfig();
    sanitizeWiFiConfig(wifiConfig);
    bleConfig = BLEConfig();
    return true;
}

bool Config::createDefaultPersonality() {
    strncpy(personalityConfig.name, "Porkchop", sizeof(personalityConfig.name) - 1);
    personalityConfig.name[sizeof(personalityConfig.name) - 1] = '\0';
    personalityConfig.mood = 50;
    personalityConfig.experience = 0;
    personalityConfig.curiosity = 0.7f;
    personalityConfig.aggression = 0.3f;
    personalityConfig.patience = 0.5f;
    personalityConfig.soundEnabled = true;
    personalityConfig.g0Action = G0Action::SCREEN_TOGGLE;
    personalityConfig.bootMode = BootMode::IDLE;
    return true;
}

void Config::setGPS(const GPSConfig& cfg) {
    gpsConfig = cfg;
    save();
}

void Config::setML(const MLConfig& cfg) {
    mlConfig = cfg;
    save();
}

void Config::setWiFi(const WiFiConfig& cfg) {
    wifiConfig = cfg;
    sanitizeWiFiConfig(wifiConfig);
    save();
}

void Config::setBLE(const BLEConfig& cfg) {
    bleConfig = cfg;
    save();
}

void Config::setPersonality(const PersonalityConfig& cfg) {
    personalityConfig = cfg;
    savePersonalityToSPIFFS();
}

bool Config::loadWpaSecKeyFromFile() {
    const char* keyFile = SDLayout::wpasecKeyPath();
    const char* legacyKeyFile = SDLayout::legacyWpasecKeyPath();

    if (!sdAvailable) {
        return false;
    }
    if (!SD.exists(keyFile) && SD.exists(legacyKeyFile)) {
        keyFile = legacyKeyFile;
    }
    if (!SD.exists(keyFile)) {
        return false;
    }

    File f = SD.open(keyFile, FILE_READ);
    if (!f) {
        Serial.println("[CONFIG] Failed to open WPA-SEC key file");
        return false;
    }

    String key = f.readStringUntil('\n');
    key.trim();
    f.close();

    if (key.length() != 32) {
        Serial.printf("[CONFIG] Invalid WPA-SEC key length: %d (expected 32)\n", key.length());
        return false;
    }

    for (int i = 0; i < 32; i++) {
        char c = key.charAt(i);
        if (!isxdigit(c)) {
            Serial.printf("[CONFIG] Invalid hex char in WPA-SEC key at position %d\n", i);
            return false;
        }
    }

    // Use strncpy to safely copy the key to the char array
    strncpy(wifiConfig.wpaSecKey, key.c_str(), sizeof(wifiConfig.wpaSecKey) - 1);
    wifiConfig.wpaSecKey[sizeof(wifiConfig.wpaSecKey) - 1] = '\0';
    save();

    if (SD.remove(keyFile)) {
        Serial.println("[CONFIG] Deleted WPA-SEC key file after import");
        SDLog::log("CFG", "WPA-SEC key imported from file");
    } else {
        Serial.println("[CONFIG] Warning: Could not delete WPA-SEC key file");
    }

    return true;
}

bool Config::loadWigleKeyFromFile() {
    const char* keyFile = SDLayout::wigleKeyPath();
    const char* legacyKeyFile = SDLayout::legacyWigleKeyPath();

    if (!sdAvailable) {
        return false;
    }
    if (!SD.exists(keyFile) && SD.exists(legacyKeyFile)) {
        keyFile = legacyKeyFile;
    }
    if (!SD.exists(keyFile)) {
        return false;
    }

    File f = SD.open(keyFile, FILE_READ);
    if (!f) {
        Serial.println("[CONFIG] Failed to open WiGLE key file");
        return false;
    }

    String content = f.readStringUntil('\n');
    content.trim();
    f.close();

    int colonPos = content.indexOf(':');
    if (colonPos < 1) {
        Serial.println("[CONFIG] Invalid WiGLE key format (expected name:token)");
        return false;
    }

    String apiName = content.substring(0, colonPos);
    String apiToken = content.substring(colonPos + 1);

    apiName.trim();
    apiToken.trim();

    if (apiName.isEmpty() || apiToken.isEmpty()) {
        Serial.println("[CONFIG] WiGLE API name or token is empty");
        return false;
    }

    // Use strncpy to safely copy strings to char arrays
    strncpy(wifiConfig.wigleApiName, apiName.c_str(), sizeof(wifiConfig.wigleApiName) - 1);
    wifiConfig.wigleApiName[sizeof(wifiConfig.wigleApiName) - 1] = '\0';
    
    strncpy(wifiConfig.wigleApiToken, apiToken.c_str(), sizeof(wifiConfig.wigleApiToken) - 1);
    wifiConfig.wigleApiToken[sizeof(wifiConfig.wigleApiToken) - 1] = '\0';
    save();

    if (SD.remove(keyFile)) {
        Serial.println("[CONFIG] Deleted WiGLE key file after import");
        SDLog::log("CFG", "WiGLE API keys imported from file");
    } else {
        Serial.println("[CONFIG] Warning: Could not delete WiGLE key file");
    }

    return true;
}
