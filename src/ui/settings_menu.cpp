// Settings menu implementation

#include "settings_menu.h"
#include "display.h"
#include "../core/config.h"
#include <M5Cardputer.h>

// Static member initialization
std::vector<SettingItem> SettingsMenu::items;
uint8_t SettingsMenu::selectedIndex = 0;
uint8_t SettingsMenu::scrollOffset = 0;
bool SettingsMenu::active = false;
bool SettingsMenu::exitRequested = false;
bool SettingsMenu::keyWasPressed = false;
bool SettingsMenu::editing = false;

void SettingsMenu::init() {
    loadFromConfig();
}

void SettingsMenu::loadFromConfig() {
    items.clear();
    
    // Sound toggle
    items.push_back({
        "Sound",
        SettingType::TOGGLE,
        Config::personality().soundEnabled ? 1 : 0,
        0, 1, 1, ""
    });
    
    // Brightness (0-100%)
    items.push_back({
        "Brightness",
        SettingType::VALUE,
        100,  // TODO: Read from display
        10, 100, 10, "%"
    });
    
    // Channel hop interval
    items.push_back({
        "CH Hop",
        SettingType::VALUE,
        (int)Config::wifi().channelHopInterval,
        100, 2000, 100, "ms"
    });
    
    // Scan duration
    items.push_back({
        "Scan Time",
        SettingType::VALUE,
        (int)Config::wifi().scanDuration,
        500, 5000, 500, "ms"
    });
    
    // Enable deauth
    items.push_back({
        "Deauth",
        SettingType::TOGGLE,
        Config::wifi().enableDeauth ? 1 : 0,
        0, 1, 1, ""
    });
    
    // GPS enabled
    items.push_back({
        "GPS",
        SettingType::TOGGLE,
        Config::gps().enabled ? 1 : 0,
        0, 1, 1, ""
    });
    
    // GPS power save
    items.push_back({
        "GPS PwrSave",
        SettingType::TOGGLE,
        Config::gps().powerSave ? 1 : 0,
        0, 1, 1, ""
    });
    
    // Save & Exit action
    items.push_back({
        "< Save & Exit >",
        SettingType::ACTION,
        0, 0, 0, 0, ""
    });
}

void SettingsMenu::saveToConfig() {
    // Sound
    auto& p = Config::personality();
    p.soundEnabled = items[0].value == 1;
    Config::setPersonality(p);
    
    // Brightness - set display directly
    M5.Display.setBrightness(items[1].value * 255 / 100);
    
    // WiFi settings
    auto& w = Config::wifi();
    w.channelHopInterval = items[2].value;
    w.scanDuration = items[3].value;
    w.enableDeauth = items[4].value == 1;
    Config::setWiFi(w);
    
    // GPS settings
    auto& g = Config::gps();
    g.enabled = items[5].value == 1;
    g.powerSave = items[6].value == 1;
    Config::setGPS(g);
    
    // Save to file
    Config::save();
}

void SettingsMenu::show() {
    active = true;
    exitRequested = false;
    selectedIndex = 0;
    scrollOffset = 0;
    editing = false;
    loadFromConfig();
}

void SettingsMenu::hide() {
    active = false;
    editing = false;
}

void SettingsMenu::update() {
    if (!active) return;
    handleInput();
}

void SettingsMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();
    
    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }
    
    if (keyWasPressed) return;
    keyWasPressed = true;
    
    auto keys = M5Cardputer.Keyboard.keysState();
    auto& item = items[selectedIndex];
    
    // Navigation with ; (up) and . (down)
    if (M5Cardputer.Keyboard.isKeyPressed(';')) {
        if (editing && item.type == SettingType::VALUE) {
            // Adjust value down when editing
            item.value = max(item.minVal, item.value - item.step);
        } else {
            // Move selection up
            editing = false;
            if (selectedIndex > 0) {
                selectedIndex--;
                if (selectedIndex < scrollOffset) {
                    scrollOffset = selectedIndex;
                }
            }
        }
    }
    
    if (M5Cardputer.Keyboard.isKeyPressed('.')) {
        if (editing && item.type == SettingType::VALUE) {
            // Adjust value up when editing
            item.value = min(item.maxVal, item.value + item.step);
        } else {
            // Move selection down
            editing = false;
            if (selectedIndex < items.size() - 1) {
                selectedIndex++;
                if (selectedIndex >= scrollOffset + VISIBLE_ITEMS) {
                    scrollOffset = selectedIndex - VISIBLE_ITEMS + 1;
                }
            }
        }
    }
    
    // Enter to select/toggle/confirm
    if (keys.enter) {
        if (item.type == SettingType::ACTION) {
            // Save & Exit
            saveToConfig();
            exitRequested = true;
        } else if (item.type == SettingType::TOGGLE) {
            // Toggle ON/OFF
            item.value = item.value == 0 ? 1 : 0;
        } else if (item.type == SettingType::VALUE) {
            if (editing) {
                // Confirm and exit edit mode
                editing = false;
            } else {
                // Enter edit mode
                editing = true;
            }
        }
    }
    
    // ESC/backtick to exit edit mode or menu
    if (M5Cardputer.Keyboard.isKeyPressed('`') || M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE)) {
        if (editing) {
            editing = false;
        } else {
            exitRequested = true;
        }
    }
}

void SettingsMenu::draw(M5Canvas& canvas) {
    canvas.setTextColor(COLOR_FG);
    canvas.setTextSize(1);
    
    int y = 2;
    int lineHeight = 16;
    
    for (uint8_t i = 0; i < VISIBLE_ITEMS && (scrollOffset + i) < items.size(); i++) {
        uint8_t idx = scrollOffset + i;
        auto& item = items[idx];
        
        bool isSelected = (idx == selectedIndex);
        
        // Highlight selected row
        if (isSelected) {
            canvas.fillRect(0, y, DISPLAY_W, lineHeight, COLOR_FG);
            canvas.setTextColor(COLOR_BG);
        } else {
            canvas.setTextColor(COLOR_FG);
        }
        
        // Draw label
        canvas.setTextDatum(top_left);
        canvas.drawString(item.label, 4, y + 2);
        
        // Draw value on the right
        String valStr;
        if (item.type == SettingType::TOGGLE) {
            valStr = item.value ? "ON" : "OFF";
        } else if (item.type == SettingType::VALUE) {
            if (isSelected && editing) {
                valStr = "[" + String(item.value) + item.suffix + "]";
            } else {
                valStr = String(item.value) + item.suffix;
            }
        } else if (item.type == SettingType::ACTION) {
            // Action - label is the whole thing, centered
            valStr = "";
        }
        
        if (valStr.length() > 0) {
            canvas.setTextDatum(top_right);
            canvas.drawString(valStr, DISPLAY_W - 4, y + 2);
        }
        
        y += lineHeight;
    }
    
    // Scroll indicators
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_center);
    if (scrollOffset > 0) {
        canvas.drawString("^", DISPLAY_W / 2, 0);
    }
    if (scrollOffset + VISIBLE_ITEMS < items.size()) {
        canvas.drawString("v", DISPLAY_W / 2, MAIN_H - 10);
    }
    
    // Help text at bottom - context sensitive
    canvas.setTextDatum(bottom_center);
    auto& currentItem = items[selectedIndex];
    if (editing && currentItem.type == SettingType::VALUE) {
        canvas.drawString(";/. adjust [Enter] OK", DISPLAY_W / 2, MAIN_H - 1);
    } else {
        canvas.drawString(";/. nav [Enter] edit", DISPLAY_W / 2, MAIN_H - 1);
    }
}
