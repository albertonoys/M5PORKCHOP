// Porkchop - ML-Enhanced Piglet Security Companion
// Main entry point
// Author: Anton Neledov

#include <M5Cardputer.h>
#include <M5Unified.h>
#include "core/porkchop.h"
#include "core/config.h"
#include "core/sdlog.h"
#include "ui/display.h"
#include "gps/gps.h"
#include "piglet/avatar.h"
#include "piglet/mood.h"
#include "ml/features.h"
#include "ml/inference.h"
#include "modes/oink.h"
#include "modes/warhog.h"

Porkchop porkchop;

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n=== PORKCHOP STARTING ===");
    
    // Init M5Cardputer hardware
    auto cfg = M5.config();
    M5.begin(cfg);
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Keyboard.begin();
    
    // Configure G0 button (GPIO0) as input with pullup
    pinMode(0, INPUT_PULLUP);
    
    // Load configuration from SD
    if (!Config::init()) {
        Serial.println("[MAIN] Config init failed, using defaults");
    }
    
    // Init SD logging (will be enabled via settings if user wants)
    SDLog::init();
    
    // Init display system
    Display::init();
    
    // Show boot splash (3 screens: OINK OINK, MY NAME IS, PORKCHOP)
    Display::showBootSplash();
    
    // Apply saved brightness
    M5.Display.setBrightness(Config::personality().brightness * 255 / 100);
    
    Display::showProgress("pig booting.", 10);

    // Initialize piglet personality
    Avatar::init();
    Mood::init();
    Display::showProgress("pig booting.", 30);

    // Initialize GPS (if enabled)
    if (Config::gps().enabled) {
        GPS::init(Config::gps().rxPin, Config::gps().txPin, Config::gps().baudRate);
        Display::showProgress("gps online.", 50);
    }

    // Initialize ML subsystem
    FeatureExtractor::init();
    MLInference::init();
    Display::showProgress("brain loaded.", 70);

    // Initialize modes
    OinkMode::init();
    WarhogMode::init();
    Display::showProgress("weapons hot.", 90);    // Init main controller
    porkchop.init();
    Display::showProgress("Ready!", 100);
    
    delay(500);
    
    Serial.println("=== PORKCHOP READY ===");
    Serial.printf("Piglet: %s\n", Config::personality().name);
}

void loop() {
    M5.update();
    M5Cardputer.update();
    
    // Update GPS
    if (Config::gps().enabled) {
        GPS::update();
    }
    
    // Update mood system
    Mood::update();
    
    // Update main controller (handles modes, input, state)
    porkchop.update();
    
    // Update ML (process any pending callbacks)
    MLInference::update();
    
    // Update display
    Display::update();
    
    // Slower update rate for smoother animation
    delay(50);
}
