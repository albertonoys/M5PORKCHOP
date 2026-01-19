
// Settings menu implementation

#include "settings_menu.h"
#include "display.h"
#include "../core/config.h"
#include "../core/sdlog.h"
#include "../gps/gps.h"
#include <M5Cardputer.h>
#include <SD.h>
#include <string.h>

namespace {
enum GroupId : uint8_t {
    GROUP_NONE = 0,
    GROUP_NET,
    GROUP_INTEG,
    GROUP_RADIO,
    GROUP_GPS,
    GROUP_BLE,
    GROUP_LOG
};

enum SettingId : uint8_t {
    SET_THEME = 0,
    SET_BRIGHTNESS,
    SET_SOUND,
    SET_DIM_AFTER,
    SET_DIM_LEVEL,
    SET_G0_ACTION,
    SET_WIFI_SSID,
    SET_WIFI_PASS,
    SET_WPASEC_STATUS,
    SET_WPASEC_LOAD,
    SET_WIGLE_NAME_STATUS,
    SET_WIGLE_TOKEN_STATUS,
    SET_WIGLE_LOAD,
    SET_CH_HOP,
    SET_LOCK_TIME,
    SET_DEAUTH,
    SET_RND_MAC,
    SET_GPS_ENABLED,
    SET_GPS_PWRSAVE,
    SET_GPS_SCAN_INTV,
    SET_GPS_BAUD,
    SET_GPS_RX,
    SET_GPS_TX,
    SET_GPS_TZ,
    SET_BLE_BURST,
    SET_BLE_ADV,
    SET_SD_LOG
};

struct RootEntry {
    const char* label;
    const char* description;
    bool isGroup;
    GroupId group;
    SettingId direct;
};

struct EntryData {
    SettingId id;
    const char* label;
    SettingType type;
    int minVal;
    int maxVal;
    int step;
    const char* suffix;
    const char* description;
};

static const EntryData kDirectEntries[] = {
    {SET_THEME, "THEME", SettingType::VALUE, 0, (int)THEME_COUNT - 1, 1, "", "CYCLE COLORS"},
    {SET_BRIGHTNESS, "BRIGHTNESS", SettingType::VALUE, 10, 100, 10, "%", "SCREEN GLOW LEVEL"},
    {SET_SOUND, "SOUND", SettingType::TOGGLE, 0, 1, 1, "", "BEEPS AND BOOPS"},
    {SET_DIM_AFTER, "DIM AFTER", SettingType::VALUE, 0, 300, 10, "S", "0 = NEVER DIM"},
    {SET_DIM_LEVEL, "DIM LEVEL", SettingType::VALUE, 0, 50, 5, "%", "0 = SCREEN OFF"},
    {SET_G0_ACTION, "G0 ACTION", SettingType::VALUE, 0, (int)G0_ACTION_COUNT - 1, 1, "", "G0 HOTKEY"}
};

static const RootEntry kRootEntries[] = {
    {"THEME", "CYCLE COLORS", false, GROUP_NONE, SET_THEME},
    {"BRIGHTNESS", "SCREEN GLOW LEVEL", false, GROUP_NONE, SET_BRIGHTNESS},
    {"SOUND", "BEEPS AND BOOPS", false, GROUP_NONE, SET_SOUND},
    {"DIM AFTER", "0 = NEVER DIM", false, GROUP_NONE, SET_DIM_AFTER},
    {"DIM LEVEL", "0 = SCREEN OFF", false, GROUP_NONE, SET_DIM_LEVEL},
    {"G0 ACTION", "G0 HOTKEY", false, GROUP_NONE, SET_G0_ACTION},
    {"NETWORK", "WIFI CREDENTIALS", true, GROUP_NET, SET_THEME},
    {"INTEG", "API KEYS", true, GROUP_INTEG, SET_THEME},
    {"RADIO", "WIFI SCAN/ATTACK TIMING", true, GROUP_RADIO, SET_THEME},
    {"GPS", "GPS MODULE SETTINGS", true, GROUP_GPS, SET_THEME},
    {"BLE", "BLE ATTACK TUNING", true, GROUP_BLE, SET_THEME}
};
static const EntryData kNetEntries[] = {
    {SET_WIFI_SSID, "WIFI SSID", SettingType::TEXT, 0, 0, 0, "", "NETWORK FOR FILE XFER"},
    {SET_WIFI_PASS, "WIFI PASS", SettingType::TEXT, 0, 0, 0, "", "SECRET SAUCE GOES HERE"}
};

static const EntryData kIntegEntries[] = {
    {SET_WPASEC_STATUS, "WPA-SEC", SettingType::TEXT, 0, 0, 0, "", "WPA-SEC.STANEV.ORG KEY"},
    {SET_WPASEC_LOAD, "KEY LOAD", SettingType::ACTION, 0, 0, 0, "", "READ /WPASEC_KEY.TXT"},
    {SET_WIGLE_NAME_STATUS, "WGL NAME", SettingType::TEXT, 0, 0, 0, "", "WIGLE.NET API NAME"},
    {SET_WIGLE_TOKEN_STATUS, "WGL TKN", SettingType::TEXT, 0, 0, 0, "", "WIGLE.NET API TOKEN"},
    {SET_WIGLE_LOAD, "WGL LOAD", SettingType::ACTION, 0, 0, 0, "", "READ /WIGLE_KEY.TXT"}
};

static const EntryData kRadioEntries[] = {
    {SET_CH_HOP, "STREET SW33P", SettingType::VALUE, 100, 2000, 100, "MS", "HOW FAST YOU SWEEP THE BAND"},
    {SET_LOCK_TIME, "GL4SS ST4R3", SettingType::VALUE, 1000, 10000, 500, "MS", "HOW LONG YOU HOLD A TARGET"},
    {SET_DEAUTH, "DEAUTH", SettingType::TOGGLE, 0, 1, 1, "", "KICK CLIENTS OFF APS"},
    {SET_RND_MAC, "RND MAC", SettingType::TOGGLE, 0, 1, 1, "", "NEW MAC EACH MODE START"}
};

static const EntryData kGpsEntries[] = {
    {SET_GPS_ENABLED, "GPS", SettingType::TOGGLE, 0, 1, 1, "", "POSITION TRACKING"},
    {SET_GPS_PWRSAVE, "PWR SAVE", SettingType::TOGGLE, 0, 1, 1, "", "SLEEP WHEN NOT HUNTING"},
    {SET_GPS_SCAN_INTV, "SCAN INTV", SettingType::VALUE, 1, 30, 1, "S", "WARHOG SCAN FREQUENCY"},
    {SET_GPS_BAUD, "GPS BAUD", SettingType::VALUE, 0, 3, 1, "", "MATCH YOUR GPS MODULE"},
    {SET_GPS_RX, "GPS RX PIN", SettingType::VALUE, 1, 46, 1, "", "G1=GROVE, G15=LORACAP"},
    {SET_GPS_TX, "GPS TX PIN", SettingType::VALUE, 1, 46, 1, "", "G2=GROVE, G13=LORACAP"},
    {SET_GPS_TZ, "TZ OFFSET", SettingType::VALUE, -12, 14, 1, "H", "TZ OFFSET"}
};

static const EntryData kBleEntries[] = {
    {SET_BLE_BURST, "BLE BURST", SettingType::VALUE, 50, 500, 50, "MS", "ATTACK SPEED"},
    {SET_BLE_ADV, "ADV TIME", SettingType::VALUE, 50, 200, 25, "MS", "PER-PACKET DURATION"}
};

// ML entries removed for heap savings

static const EntryData kLogEntries[] = {
    {SET_SD_LOG, "SD LOG", SettingType::TOGGLE, 0, 1, 1, "", "DEBUG SPAM TO SD"}
};

static const char* const kG0ActionLabels[G0_ACTION_COUNT] = {
    "SCREEN",
    "OINK",
    "DNHAM",
    "SPECTRM",
    "PIGSYNC"
};

static const uint32_t kGpsBaudRates[] = {9600, 38400, 57600, 115200};

static int clampValue(int value, int minVal, int maxVal) {
    if (value < minVal) return minVal;
    if (value > maxVal) return maxVal;
    return value;
}

static bool isTextEditable(SettingId id) {
    return id == SET_WIFI_SSID || id == SET_WIFI_PASS;
}

static bool isPersonalitySetting(SettingId id) {
    return id == SET_THEME || id == SET_BRIGHTNESS || id == SET_SOUND ||
           id == SET_DIM_AFTER || id == SET_DIM_LEVEL || id == SET_G0_ACTION;
}

static bool isConfigSetting(SettingId id) {
    switch (id) {
        case SET_WIFI_SSID:
        case SET_WIFI_PASS:
        case SET_CH_HOP:
        case SET_LOCK_TIME:
        case SET_DEAUTH:
        case SET_RND_MAC:
        case SET_GPS_ENABLED:
        case SET_GPS_PWRSAVE:
        case SET_GPS_SCAN_INTV:
        case SET_GPS_BAUD:
        case SET_GPS_RX:
        case SET_GPS_TX:
        case SET_GPS_TZ:
        case SET_BLE_BURST:
        case SET_BLE_ADV:
            return true;
        default:
            return false;
    }
}

static const EntryData* findDirectEntry(SettingId id) {
    for (size_t i = 0; i < sizeof(kDirectEntries) / sizeof(kDirectEntries[0]); ++i) {
        if (kDirectEntries[i].id == id) {
            return &kDirectEntries[i];
        }
    }
    return nullptr;
}

static const EntryData* getGroupEntries(GroupId group, size_t* count) {
    switch (group) {
        case GROUP_NET:
            *count = sizeof(kNetEntries) / sizeof(kNetEntries[0]);
            return kNetEntries;
        case GROUP_INTEG:
            *count = sizeof(kIntegEntries) / sizeof(kIntegEntries[0]);
            return kIntegEntries;
        case GROUP_RADIO:
            *count = sizeof(kRadioEntries) / sizeof(kRadioEntries[0]);
            return kRadioEntries;
        case GROUP_GPS:
            *count = sizeof(kGpsEntries) / sizeof(kGpsEntries[0]);
            return kGpsEntries;
        case GROUP_BLE:
            *count = sizeof(kBleEntries) / sizeof(kBleEntries[0]);
            return kBleEntries;
        case GROUP_LOG:
            *count = sizeof(kLogEntries) / sizeof(kLogEntries[0]);
            return kLogEntries;
        default:
            *count = 0;
            return nullptr;
    }
}

static const char* getGroupLabel(GroupId group) {
    switch (group) {
        case GROUP_NET:
            return "NETWORK";
        case GROUP_INTEG:
            return "INTEG";
        case GROUP_RADIO:
            return "RADIO";
        case GROUP_GPS:
            return "GPS";
        case GROUP_BLE:
            return "BLE";
        case GROUP_LOG:
            return "LOG";
        default:
            return "SETTINGS";
    }
}

static int getGpsBaudIndex() {
    uint32_t baud = Config::gps().baudRate;
    for (int i = 0; i < 4; ++i) {
        if (baud == kGpsBaudRates[i]) {
            return i;
        }
    }
    return 3;
}

static uint32_t getGpsBaudForIndex(int index) {
    if (index < 0) index = 0;
    if (index > 3) index = 3;
    return kGpsBaudRates[index];
}

static void formatWpaSecStatus(char* out, size_t len) {
    if (!out || len == 0) return;
    const char* key = Config::wifi().wpaSecKey.c_str();
    size_t keyLen = strlen(key);
    if (keyLen == 0) {
        strncpy(out, "UNSET", len - 1);
        out[len - 1] = '\0';
        return;
    }
    if (keyLen < 8) {
        size_t keep = (keyLen < 2) ? keyLen : 2;
        if (keep >= len) keep = len - 1;
        memcpy(out, key, keep);
        if (keep + 3 < len) {
            memcpy(out + keep, "...", 3);
            out[keep + 3] = '\0';
        } else {
            out[keep] = '\0';
        }
        return;
    }
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%.3s...%.2s", key, key + keyLen - 2);
    strncpy(out, tmp, len - 1);
    out[len - 1] = '\0';
}

static void formatWigleNameStatus(char* out, size_t len) {
    if (!out || len == 0) return;
    const char* name = Config::wifi().wigleApiName.c_str();
    size_t nameLen = strlen(name);
    if (nameLen == 0) {
        strncpy(out, "UNSET", len - 1);
        out[len - 1] = '\0';
        return;
    }
    if (nameLen <= 3) {
        strncpy(out, name, len - 1);
        out[len - 1] = '\0';
        return;
    }
    size_t keep = 3;
    if (keep >= len) keep = len - 1;
    memcpy(out, name, keep);
    if (keep + 3 < len) {
        memcpy(out + keep, "...", 3);
        out[keep + 3] = '\0';
    } else {
        out[keep] = '\0';
    }
}

static void formatWigleTokenStatus(char* out, size_t len) {
    if (!out || len == 0) return;
    const char* token = Config::wifi().wigleApiToken.c_str();
    size_t tokenLen = strlen(token);
    if (tokenLen == 0) {
        strncpy(out, "UNSET", len - 1);
        out[len - 1] = '\0';
        return;
    }
    if (tokenLen < 8) {
        size_t keep = (tokenLen < 2) ? tokenLen : 2;
        if (keep >= len) keep = len - 1;
        memcpy(out, token, keep);
        if (keep + 3 < len) {
            memcpy(out + keep, "...", 3);
            out[keep + 3] = '\0';
        } else {
            out[keep] = '\0';
        }
        return;
    }
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%.3s...%.2s", token, token + tokenLen - 2);
    strncpy(out, tmp, len - 1);
    out[len - 1] = '\0';
}

static void getSettingTextBuf(SettingId id, char* out, size_t len) {
    if (!out || len == 0) return;
    out[0] = '\0';
    switch (id) {
        case SET_WIFI_SSID:
            strncpy(out, Config::wifi().otaSSID.c_str(), len - 1);
            out[len - 1] = '\0';
            return;
        case SET_WIFI_PASS:
            strncpy(out, Config::wifi().otaPassword.c_str(), len - 1);
            out[len - 1] = '\0';
            return;
        case SET_WPASEC_STATUS:
            formatWpaSecStatus(out, len);
            return;
        case SET_WIGLE_NAME_STATUS:
            formatWigleNameStatus(out, len);
            return;
        case SET_WIGLE_TOKEN_STATUS:
            formatWigleTokenStatus(out, len);
            return;
        default:
            out[0] = '\0';
            return;
    }
}

static void formatTruncated(const char* src, char* out, size_t len, size_t maxChars, const char* ellipsis) {
    if (!out || len == 0) return;
    if (!src) {
        out[0] = '\0';
        return;
    }
    size_t srcLen = strlen(src);
    size_t limit = maxChars;
    if (limit >= len) limit = len - 1;
    if (limit == 0) {
        out[0] = '\0';
        return;
    }
    size_t ellLen = (ellipsis ? strlen(ellipsis) : 0);
    if (srcLen > limit && ellLen < limit) {
        size_t keep = limit - ellLen;
        memcpy(out, src, keep);
        if (ellipsis && keep + ellLen < len) {
            memcpy(out + keep, ellipsis, ellLen);
            out[keep + ellLen] = '\0';
        } else {
            out[keep] = '\0';
        }
        return;
    }
    size_t copyLen = (srcLen < len - 1) ? srcLen : (len - 1);
    memcpy(out, src, copyLen);
    out[copyLen] = '\0';
}

static const char* getG0ActionLabel(int idx) {
    if (idx < 0 || idx >= (int)G0_ACTION_COUNT) {
        return kG0ActionLabels[0];
    }
    return kG0ActionLabels[idx];
}

static int getSettingValue(SettingId id) {
    switch (id) {
        case SET_THEME:
            return Config::personality().themeIndex;
        case SET_BRIGHTNESS:
            return Config::personality().brightness;
        case SET_SOUND:
            return Config::personality().soundEnabled ? 1 : 0;
        case SET_DIM_AFTER:
            return Config::personality().dimTimeout;
        case SET_DIM_LEVEL:
            return Config::personality().dimLevel;
        case SET_G0_ACTION:
            return static_cast<int>(Config::personality().g0Action);
        case SET_CH_HOP:
            return Config::wifi().channelHopInterval;
        case SET_LOCK_TIME:
            return Config::wifi().lockTime;
        case SET_DEAUTH:
            return Config::wifi().enableDeauth ? 1 : 0;
        case SET_RND_MAC:
            return Config::wifi().randomizeMAC ? 1 : 0;
        case SET_GPS_ENABLED:
            return Config::gps().enabled ? 1 : 0;
        case SET_GPS_PWRSAVE:
            return Config::gps().powerSave ? 1 : 0;
        case SET_GPS_SCAN_INTV:
            return Config::gps().updateInterval;
        case SET_GPS_BAUD:
            return getGpsBaudIndex();
        case SET_GPS_RX:
            return Config::gps().rxPin;
        case SET_GPS_TX:
            return Config::gps().txPin;
        case SET_GPS_TZ:
            return Config::gps().timezoneOffset;
        case SET_BLE_BURST:
            return Config::ble().burstInterval;
        case SET_BLE_ADV:
            return Config::ble().advDuration;
        case SET_SD_LOG:
            return SDLog::isEnabled() ? 1 : 0;
        default:
            return 0;
    }
}

static bool setSettingValue(SettingId id, int value) {
    switch (id) {
        case SET_THEME: {
            uint8_t newVal = static_cast<uint8_t>(value);
            if (Config::personality().themeIndex == newVal) return false;
            Config::personality().themeIndex = newVal;
            return true;
        }
        case SET_BRIGHTNESS: {
            uint8_t newVal = static_cast<uint8_t>(value);
            if (Config::personality().brightness == newVal) return false;
            Config::personality().brightness = newVal;
            Display::resetDimTimer();
            M5.Display.setBrightness(newVal * 255 / 100);
            return true;
        }
        case SET_SOUND: {
            bool enabled = value != 0;
            if (Config::personality().soundEnabled == enabled) return false;
            Config::personality().soundEnabled = enabled;
            return true;
        }
        case SET_DIM_AFTER: {
            uint16_t newVal = static_cast<uint16_t>(value);
            if (Config::personality().dimTimeout == newVal) return false;
            Config::personality().dimTimeout = newVal;
            Display::resetDimTimer();
            return true;
        }
        case SET_DIM_LEVEL: {
            uint8_t newVal = static_cast<uint8_t>(value);
            if (Config::personality().dimLevel == newVal) return false;
            Config::personality().dimLevel = newVal;
            Display::resetDimTimer();
            return true;
        }
        case SET_G0_ACTION: {
            uint8_t newVal = static_cast<uint8_t>(value);
            if (newVal >= G0_ACTION_COUNT) {
                newVal = static_cast<uint8_t>(G0Action::SCREEN_TOGGLE);
            }
            if (Config::personality().g0Action == static_cast<G0Action>(newVal)) return false;
            Config::personality().g0Action = static_cast<G0Action>(newVal);
            return true;
        }
        case SET_CH_HOP: {
            uint16_t newVal = static_cast<uint16_t>(value);
            if (Config::wifi().channelHopInterval == newVal) return false;
            Config::wifi().channelHopInterval = newVal;
            return true;
        }
        case SET_LOCK_TIME: {
            uint16_t newVal = static_cast<uint16_t>(value);
            if (Config::wifi().lockTime == newVal) return false;
            Config::wifi().lockTime = newVal;
            return true;
        }
        case SET_DEAUTH: {
            bool enabled = value != 0;
            if (Config::wifi().enableDeauth == enabled) return false;
            Config::wifi().enableDeauth = enabled;
            return true;
        }
        case SET_RND_MAC: {
            bool enabled = value != 0;
            if (Config::wifi().randomizeMAC == enabled) return false;
            Config::wifi().randomizeMAC = enabled;
            return true;
        }
        case SET_GPS_ENABLED: {
            bool enabled = value != 0;
            if (Config::gps().enabled == enabled) return false;
            Config::gps().enabled = enabled;
            return true;
        }
        case SET_GPS_PWRSAVE: {
            bool enabled = value != 0;
            if (Config::gps().powerSave == enabled) return false;
            Config::gps().powerSave = enabled;
            return true;
        }
        case SET_GPS_SCAN_INTV: {
            uint16_t newVal = static_cast<uint16_t>(value);
            if (Config::gps().updateInterval == newVal) return false;
            Config::gps().updateInterval = newVal;
            return true;
        }
        case SET_GPS_BAUD: {
            uint32_t newBaud = getGpsBaudForIndex(value);
            if (Config::gps().baudRate == newBaud) return false;
            Config::gps().baudRate = newBaud;
            return true;
        }
        case SET_GPS_RX: {
            uint8_t newVal = static_cast<uint8_t>(value);
            if (Config::gps().rxPin == newVal) return false;
            Config::gps().rxPin = newVal;
            return true;
        }
        case SET_GPS_TX: {
            uint8_t newVal = static_cast<uint8_t>(value);
            if (Config::gps().txPin == newVal) return false;
            Config::gps().txPin = newVal;
            return true;
        }
        case SET_GPS_TZ: {
            int8_t newVal = static_cast<int8_t>(value);
            if (Config::gps().timezoneOffset == newVal) return false;
            Config::gps().timezoneOffset = newVal;
            return true;
        }
        case SET_BLE_BURST: {
            uint16_t newVal = static_cast<uint16_t>(value);
            if (Config::ble().burstInterval == newVal) return false;
            Config::ble().burstInterval = newVal;
            return true;
        }
        case SET_BLE_ADV: {
            uint16_t newVal = static_cast<uint16_t>(value);
            if (Config::ble().advDuration == newVal) return false;
            Config::ble().advDuration = newVal;
            return true;
        }
        case SET_SD_LOG: {
            bool enabled = value != 0;
            if (SDLog::isEnabled() == enabled) return false;
            SDLog::setEnabled(enabled);
            return true;
        }
        default:
            return false;
    }
}

static bool setSettingText(SettingId id, const String& value) {
    switch (id) {
        case SET_WIFI_SSID:
            if (Config::wifi().otaSSID == value) return false;
            Config::wifi().otaSSID = value;
            return true;
        case SET_WIFI_PASS:
            if (Config::wifi().otaPassword == value) return false;
            Config::wifi().otaPassword = value;
            return true;
        default:
            return false;
    }
}

static String getSettingText(SettingId id) {
    char buf[48];
    getSettingTextBuf(id, buf, sizeof(buf));
    return String(buf);
}
}  // namespace
bool SettingsMenu::active = false;
bool SettingsMenu::exitRequested = false;
bool SettingsMenu::keyWasPressed = false;
bool SettingsMenu::editing = false;
bool SettingsMenu::textEditing = false;
String SettingsMenu::textBuffer = "";
uint8_t SettingsMenu::rootIndex = 0;
uint8_t SettingsMenu::rootScroll = 0;
uint8_t SettingsMenu::groupIndex = 0;
uint8_t SettingsMenu::groupScroll = 0;
uint8_t SettingsMenu::activeGroup = GROUP_NONE;
uint8_t SettingsMenu::textEditId = 0;
uint32_t SettingsMenu::lastInputMs = 0;
bool SettingsMenu::dirtyConfig = false;
bool SettingsMenu::dirtyPersonality = false;
uint8_t SettingsMenu::origGpsRxPin = 0;
uint8_t SettingsMenu::origGpsTxPin = 0;
uint32_t SettingsMenu::origGpsBaud = 0;

void SettingsMenu::init() {
    active = false;
    exitRequested = false;
}

void SettingsMenu::show() {
    active = true;
    exitRequested = false;
    keyWasPressed = true;
    editing = false;
    textEditing = false;
    textBuffer = "";
    rootIndex = 0;
    rootScroll = 0;
    groupIndex = 0;
    groupScroll = 0;
    activeGroup = GROUP_NONE;
    textEditId = 0;
    lastInputMs = millis();
    dirtyConfig = false;
    dirtyPersonality = false;

    origGpsRxPin = Config::gps().rxPin;
    origGpsTxPin = Config::gps().txPin;
    origGpsBaud = Config::gps().baudRate;
}

void SettingsMenu::hide() {
    saveIfDirty(false);
    active = false;
    editing = false;
    textEditing = false;
}

void SettingsMenu::update() {
    if (!active) return;
    handleInput();
    maybeAutoSave();
}

void SettingsMenu::maybeAutoSave() {
    if (!dirtyConfig && !dirtyPersonality) return;
    if (editing || textEditing) return;
    if (millis() - lastInputMs < AUTO_SAVE_MS) return;
    saveIfDirty(false);
}

void SettingsMenu::saveIfDirty(bool showToast) {
    if (!dirtyConfig && !dirtyPersonality) return;

    if (dirtyConfig) {
        Config::save();
    }

    if (dirtyPersonality) {
        Config::setPersonality(Config::personality());
    }

    if (dirtyConfig) {
        bool gpsChanged = (Config::gps().rxPin != origGpsRxPin) ||
                          (Config::gps().txPin != origGpsTxPin) ||
                          (Config::gps().baudRate != origGpsBaud);
        if (gpsChanged) {
            origGpsRxPin = Config::gps().rxPin;
            origGpsTxPin = Config::gps().txPin;
            origGpsBaud = Config::gps().baudRate;
            if (Config::gps().enabled) {
                GPS::reinit(origGpsRxPin, origGpsTxPin, origGpsBaud);
                if (showToast) {
                    Display::notify(NoticeKind::STATUS, "GPS reinit");
                }
            }
        }
    }

    dirtyConfig = false;
    dirtyPersonality = false;

    if (showToast) {
        Display::notify(NoticeKind::STATUS, "SAVED");
    }
}
void SettingsMenu::handleInput() {
    bool anyPressed = M5Cardputer.Keyboard.isPressed();

    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }

    if (textEditing) {
        handleTextInput();
        return;
    }

    if (keyWasPressed) return;
    keyWasPressed = true;

    lastInputMs = millis();

    auto keys = M5Cardputer.Keyboard.keysState();
    bool up = M5Cardputer.Keyboard.isKeyPressed(';');
    bool down = M5Cardputer.Keyboard.isKeyPressed('.');
    bool back = M5Cardputer.Keyboard.isKeyPressed(KEY_BACKSPACE);

    GroupId group = static_cast<GroupId>(activeGroup);
    const size_t rootCount = sizeof(kRootEntries) / sizeof(kRootEntries[0]);

    if (up || down) {
        if (editing) {
            SettingId id;
            int minVal = 0;
            int maxVal = 0;
            int step = 1;
            SettingType type = SettingType::VALUE;

            if (group == GROUP_NONE) {
                const RootEntry& entry = kRootEntries[rootIndex];
                const EntryData* direct = findDirectEntry(entry.direct);
                if (direct) {
                    id = direct->id;
                    minVal = direct->minVal;
                    maxVal = direct->maxVal;
                    step = direct->step;
                    type = direct->type;
                } else {
                    editing = false;
                    return;
                }
            } else {
                size_t count = 0;
                const EntryData* entries = getGroupEntries(group, &count);
                if (!entries || groupIndex >= count) {
                    editing = false;
                    return;
                }
                const EntryData& entry = entries[groupIndex];
                id = entry.id;
                minVal = entry.minVal;
                maxVal = entry.maxVal;
                step = entry.step;
                type = entry.type;
            }

            if (type == SettingType::VALUE) {
                int current = getSettingValue(id);
                int next = current + (up ? step : -step);
                next = clampValue(next, minVal, maxVal);
                if (setSettingValue(id, next)) {
                    if (isPersonalitySetting(id)) dirtyPersonality = true;
                    if (isConfigSetting(id)) dirtyConfig = true;
                    lastInputMs = millis();
                }
            }
            return;
        }

        if (group == GROUP_NONE) {
            editing = false;
            if (up && rootIndex > 0) {
                rootIndex--;
            } else if (down && rootIndex + 1 < rootCount) {
                rootIndex++;
            }

            if (rootIndex < rootScroll) {
                rootScroll = rootIndex;
            } else if (rootIndex >= rootScroll + VISIBLE_ROOT_ITEMS) {
                rootScroll = rootIndex - VISIBLE_ROOT_ITEMS + 1;
            }
        } else {
            size_t count = 0;
            const EntryData* entries = getGroupEntries(group, &count);
            if (!entries) return;
            editing = false;

            if (up && groupIndex > 0) {
                groupIndex--;
            } else if (down && groupIndex + 1 < count) {
                groupIndex++;
            }

            if (groupIndex < groupScroll) {
                groupScroll = groupIndex;
            } else if (groupIndex >= groupScroll + VISIBLE_GROUP_ITEMS) {
                groupScroll = groupIndex - VISIBLE_GROUP_ITEMS + 1;
            }
        }
    }

    if (keys.enter) {
        if (group == GROUP_NONE) {
            const RootEntry& entry = kRootEntries[rootIndex];
            if (entry.isGroup) {
                activeGroup = entry.group;
                groupIndex = 0;
                groupScroll = 0;
                editing = false;
            } else {
                const EntryData* direct = findDirectEntry(entry.direct);
                if (!direct) return;

                if (direct->type == SettingType::TOGGLE) {
                    int next = getSettingValue(direct->id) ? 0 : 1;
                    if (setSettingValue(direct->id, next)) {
                        if (isPersonalitySetting(direct->id)) dirtyPersonality = true;
                        if (isConfigSetting(direct->id)) dirtyConfig = true;
                    }
                } else if (direct->type == SettingType::VALUE) {
                    editing = !editing;
                }
            }
        } else {
            size_t count = 0;
            const EntryData* entries = getGroupEntries(group, &count);
            if (!entries || groupIndex >= count) return;
            const EntryData& entry = entries[groupIndex];

            switch (entry.type) {
                case SettingType::ACTION:
                    if (entry.id == SET_WPASEC_LOAD) {
                        if (Config::loadWpaSecKeyFromFile()) {
                            Display::notify(NoticeKind::STATUS, "Key loaded");
                        } else if (!Config::isSDAvailable()) {
                            Display::notify(NoticeKind::WARNING, "No SD card");
                        } else if (!SD.exists("/wpasec_key.txt")) {
                            Display::notify(NoticeKind::WARNING, "No key file");
                        } else {
                            Display::notify(NoticeKind::WARNING, "Invalid key");
                        }
                    } else if (entry.id == SET_WIGLE_LOAD) {
                        if (Config::loadWigleKeyFromFile()) {
                            Display::notify(NoticeKind::STATUS, "WiGLE key loaded");
                        } else if (!Config::isSDAvailable()) {
                            Display::notify(NoticeKind::WARNING, "No SD card");
                        } else if (!SD.exists("/wigle_key.txt")) {
                            Display::notify(NoticeKind::WARNING, "No key file");
                        } else {
                            Display::notify(NoticeKind::WARNING, "Invalid format");
                        }
                    }
                    break;
                case SettingType::TOGGLE: {
                    int next = getSettingValue(entry.id) ? 0 : 1;
                    if (setSettingValue(entry.id, next)) {
                        if (isPersonalitySetting(entry.id)) dirtyPersonality = true;
                        if (isConfigSetting(entry.id)) dirtyConfig = true;
                    }
                    break;
                }
                case SettingType::VALUE:
                    editing = !editing;
                    break;
                case SettingType::TEXT:
                    if (isTextEditable(entry.id)) {
                        textEditing = true;
                        textBuffer = getSettingText(entry.id);
                        textEditId = entry.id;
                        keyWasPressed = true;
                    }
                    break;
            }
        }
    }

    if (back) {
        if (editing) {
            editing = false;
        } else if (group != GROUP_NONE) {
            activeGroup = GROUP_NONE;
            groupIndex = 0;
            groupScroll = 0;
        } else {
            saveIfDirty(true);
            exitRequested = true;
        }
    }
}
void SettingsMenu::handleTextInput() {
    auto keys = M5Cardputer.Keyboard.keysState();
    bool anyPressed = M5Cardputer.Keyboard.isPressed();

    if (!anyPressed) {
        keyWasPressed = false;
        return;
    }

    bool hasPrintable = !keys.word.empty();
    bool hasActionKey = keys.enter || keys.del;

    if (!hasPrintable && !hasActionKey) {
        return;
    }

    if (keyWasPressed) return;
    keyWasPressed = true;

    lastInputMs = millis();

    if (keys.enter) {
        bool changed = setSettingText(static_cast<SettingId>(textEditId), textBuffer);
        if (changed) {
            dirtyConfig = true;
        }
        textEditing = false;
        textBuffer = "";
        return;
    }

    if (keys.del) {
        if (textBuffer.length() > 0) {
            textBuffer.remove(textBuffer.length() - 1);
        }
        return;
    }

    for (char c : keys.word) {
        if (c == '`') {
            textEditing = false;
            textBuffer = "";
            return;
        }
    }

    if (textBuffer.length() < 32) {
        for (char c : keys.word) {
            if (c >= 32 && c <= 126 && c != '`' && textBuffer.length() < 32) {
                textBuffer += c;
            }
        }
    }
}

const char* SettingsMenu::getSelectedDescription() {
    if (!active) return "";

    GroupId group = static_cast<GroupId>(activeGroup);
    if (group == GROUP_NONE) {
        return kRootEntries[rootIndex].description;
    }

    size_t count = 0;
    const EntryData* entries = getGroupEntries(group, &count);
    if (!entries || groupIndex >= count) return "";
    return entries[groupIndex].description;
}

void SettingsMenu::draw(M5Canvas& canvas) {
    canvas.fillSprite(COLOR_FG);
    canvas.setTextColor(COLOR_BG);
    canvas.setTextSize(2);

    const int lineHeight = 18;
    GroupId group = static_cast<GroupId>(activeGroup);

    if (group == GROUP_NONE) {
        const size_t rootCount = sizeof(kRootEntries) / sizeof(kRootEntries[0]);
        int y = 2;

        for (uint8_t i = 0; i < VISIBLE_ROOT_ITEMS && (rootScroll + i) < rootCount; i++) {
            uint8_t idx = rootScroll + i;
            const RootEntry& entry = kRootEntries[idx];
            bool selected = (idx == rootIndex);

            if (selected) {
                canvas.fillRect(0, y, DISPLAY_W, lineHeight, COLOR_BG);
                canvas.setTextColor(COLOR_FG);
            } else {
                canvas.setTextColor(COLOR_BG);
            }

            canvas.setTextDatum(top_left);
            canvas.drawString(entry.label, 4, y + 2);

            char valBuf[32];
            valBuf[0] = '\0';
            if (entry.isGroup) {
                strncpy(valBuf, ">", sizeof(valBuf) - 1);
                valBuf[sizeof(valBuf) - 1] = '\0';
            } else {
                const EntryData* direct = findDirectEntry(entry.direct);
                if (direct) {
                    int value = getSettingValue(direct->id);
                    if (direct->type == SettingType::TOGGLE) {
                        strncpy(valBuf, value ? "ON" : "OFF", sizeof(valBuf) - 1);
                        valBuf[sizeof(valBuf) - 1] = '\0';
                    } else if (direct->id == SET_THEME) {
                        int idxValue = clampValue(value, 0, (int)THEME_COUNT - 1);
                        char themeBuf[16];
                        formatTruncated(THEMES[idxValue].name, themeBuf, sizeof(themeBuf), 8, "...");
                        if (selected && editing) {
                            snprintf(valBuf, sizeof(valBuf), "[%s]", themeBuf);
                        } else {
                            strncpy(valBuf, themeBuf, sizeof(valBuf) - 1);
                            valBuf[sizeof(valBuf) - 1] = '\0';
                        }
                    } else if (direct->id == SET_G0_ACTION) {
                        const char* actionLabel = getG0ActionLabel(value);
                        if (selected && editing) {
                            snprintf(valBuf, sizeof(valBuf), "[%s]", actionLabel);
                        } else {
                            strncpy(valBuf, actionLabel, sizeof(valBuf) - 1);
                            valBuf[sizeof(valBuf) - 1] = '\0';
                        }
                    } else if (selected && editing) {
                        snprintf(valBuf, sizeof(valBuf), "[%d%s]", value, direct->suffix);
                    } else {
                        snprintf(valBuf, sizeof(valBuf), "%d%s", value, direct->suffix);
                    }
                }
            }

            if (valBuf[0] != '\0') {
                canvas.setTextDatum(top_right);
                canvas.drawString(valBuf, DISPLAY_W - 4, y + 2);
            }

            y += lineHeight;
        }

        canvas.setTextColor(COLOR_BG);
        canvas.setTextDatum(top_center);
        if (rootScroll > 0) {
            canvas.drawString("^", DISPLAY_W / 2, 0);
        }
        if (rootScroll + VISIBLE_ROOT_ITEMS < rootCount) {
            canvas.drawString("v", DISPLAY_W / 2, MAIN_H - 10);
        }
        return;
    }

    size_t count = 0;
    const EntryData* entries = getGroupEntries(group, &count);
    if (!entries) return;

    canvas.fillRect(0, 0, DISPLAY_W, lineHeight, COLOR_BG);
    canvas.setTextColor(COLOR_FG);
    canvas.setTextDatum(top_left);
    canvas.drawString(getGroupLabel(group), 4, 2);

    int y = lineHeight + 2;
    for (uint8_t i = 0; i < VISIBLE_GROUP_ITEMS && (groupScroll + i) < count; i++) {
        uint8_t idx = groupScroll + i;
        const EntryData& entry = entries[idx];
        bool selected = (idx == groupIndex);

        if (selected) {
            canvas.fillRect(0, y, DISPLAY_W, lineHeight, COLOR_BG);
            canvas.setTextColor(COLOR_FG);
        } else {
            canvas.setTextColor(COLOR_BG);
        }

        canvas.setTextDatum(top_left);
        canvas.drawString(entry.label, 4, y + 2);

        char valBuf[32];
        valBuf[0] = '\0';
        if (entry.type == SettingType::ACTION) {
            strncpy(valBuf, "[EXEC]", sizeof(valBuf) - 1);
            valBuf[sizeof(valBuf) - 1] = '\0';
        } else if (entry.type == SettingType::TEXT) {
            if (selected && textEditing && entry.id == static_cast<SettingId>(textEditId)) {
                char displayBuf[12];
                const char* textSrc = textBuffer.c_str();
                size_t textLen = textBuffer.length();
                if (textLen > 5) {
                    if (textLen >= 2) {
                        snprintf(displayBuf, sizeof(displayBuf), "...%c%c",
                                 textSrc[textLen - 2], textSrc[textLen - 1]);
                    } else {
                        strncpy(displayBuf, "...", sizeof(displayBuf) - 1);
                        displayBuf[sizeof(displayBuf) - 1] = '\0';
                    }
                } else {
                    strncpy(displayBuf, textSrc, sizeof(displayBuf) - 1);
                    displayBuf[sizeof(displayBuf) - 1] = '\0';
                }
                snprintf(valBuf, sizeof(valBuf), "[%s_]", displayBuf);
            } else {
                char valueBuf[32];
                getSettingTextBuf(entry.id, valueBuf, sizeof(valueBuf));
                if (entry.id == SET_WIFI_PASS) {
                    if (valueBuf[0] == '\0') {
                        strncpy(valBuf, "<empty>", sizeof(valBuf) - 1);
                    } else {
                        strncpy(valBuf, "****", sizeof(valBuf) - 1);
                    }
                } else if (valueBuf[0] == '\0') {
                    strncpy(valBuf, "<empty>", sizeof(valBuf) - 1);
                } else if (strlen(valueBuf) > 8) {
                    char shortBuf[16];
                    formatTruncated(valueBuf, shortBuf, sizeof(shortBuf), 8, "...");
                    strncpy(valBuf, shortBuf, sizeof(valBuf) - 1);
                } else {
                    strncpy(valBuf, valueBuf, sizeof(valBuf) - 1);
                }
                valBuf[sizeof(valBuf) - 1] = '\0';
            }
        } else if (entry.type == SettingType::TOGGLE) {
            strncpy(valBuf, getSettingValue(entry.id) ? "ON" : "OFF", sizeof(valBuf) - 1);
            valBuf[sizeof(valBuf) - 1] = '\0';
        } else if (entry.type == SettingType::VALUE) {
            int value = getSettingValue(entry.id);
            if (entry.id == SET_GPS_BAUD) {
                char baudBuf[12];
                snprintf(baudBuf, sizeof(baudBuf), "%lu", (unsigned long)getGpsBaudForIndex(value));
                if (selected && editing) {
                    snprintf(valBuf, sizeof(valBuf), "[%s]", baudBuf);
                } else {
                    strncpy(valBuf, baudBuf, sizeof(valBuf) - 1);
                    valBuf[sizeof(valBuf) - 1] = '\0';
                }
            } else if (selected && editing) {
                snprintf(valBuf, sizeof(valBuf), "[%d%s]", value, entry.suffix);
            } else {
                snprintf(valBuf, sizeof(valBuf), "%d%s", value, entry.suffix);
            }
        }

        if (valBuf[0] != '\0') {
            canvas.setTextDatum(top_right);
            canvas.drawString(valBuf, DISPLAY_W - 4, y + 2);
        }

        y += lineHeight;
    }

    canvas.setTextColor(COLOR_BG);
    canvas.setTextDatum(top_center);
    if (groupScroll > 0) {
        canvas.drawString("^", DISPLAY_W / 2, lineHeight);
    }
    if (groupScroll + VISIBLE_GROUP_ITEMS < count) {
        canvas.drawString("v", DISPLAY_W / 2, MAIN_H - 10);
    }
}
