#include "SettingsRegistry.h"
#include <string.h>
#include <stdlib.h>

SettingsRegistry::SettingsRegistry() : numSettings_(0) {
    memset(settings_, 0, sizeof(settings_));
}

void SettingsRegistry::begin() {
    // Nothing to initialize currently
}

bool SettingsRegistry::registerSetting(const Setting& setting) {
    if (numSettings_ >= MAX_SETTINGS) {
        Serial.println(F("ERROR: Settings registry full"));
        return false;
    }
    settings_[numSettings_++] = setting;
    return true;
}

bool SettingsRegistry::registerUint8(const char* name, uint8_t* value, const char* category,
                                      const char* desc, uint8_t minVal, uint8_t maxVal,
                                      SettingCallback onChange, bool persistent) {
    Setting s = {name, category, desc, SettingType::UINT8, value,
                 (float)minVal, (float)maxVal, onChange, persistent};
    return registerSetting(s);
}

bool SettingsRegistry::registerInt8(const char* name, int8_t* value, const char* category,
                                     const char* desc, int8_t minVal, int8_t maxVal,
                                     SettingCallback onChange, bool persistent) {
    Setting s = {name, category, desc, SettingType::INT8, value,
                 (float)minVal, (float)maxVal, onChange, persistent};
    return registerSetting(s);
}

bool SettingsRegistry::registerUint16(const char* name, uint16_t* value, const char* category,
                                       const char* desc, uint16_t minVal, uint16_t maxVal,
                                       SettingCallback onChange, bool persistent) {
    Setting s = {name, category, desc, SettingType::UINT16, value,
                 (float)minVal, (float)maxVal, onChange, persistent};
    return registerSetting(s);
}

bool SettingsRegistry::registerUint32(const char* name, uint32_t* value, const char* category,
                                       const char* desc, uint32_t minVal, uint32_t maxVal,
                                       SettingCallback onChange, bool persistent) {
    Setting s = {name, category, desc, SettingType::UINT32, value,
                 (float)minVal, (float)maxVal, onChange, persistent};
    return registerSetting(s);
}

bool SettingsRegistry::registerFloat(const char* name, float* value, const char* category,
                                      const char* desc, float minVal, float maxVal,
                                      SettingCallback onChange, bool persistent) {
    Setting s = {name, category, desc, SettingType::FLOAT, value,
                 minVal, maxVal, onChange, persistent};
    return registerSetting(s);
}

bool SettingsRegistry::registerBool(const char* name, bool* value, const char* category,
                                     const char* desc, SettingCallback onChange, bool persistent) {
    Setting s = {name, category, desc, SettingType::BOOL, value,
                 0.0f, 1.0f, onChange, persistent};
    return registerSetting(s);
}

Setting* SettingsRegistry::findSetting(const char* name) {
    for (uint8_t i = 0; i < numSettings_; i++) {
        if (strcasecmp(settings_[i].name, name) == 0) {
            return &settings_[i];
        }
    }
    return nullptr;
}

const Setting* SettingsRegistry::getSetting(uint8_t index) const {
    if (index < numSettings_) {
        return &settings_[index];
    }
    return nullptr;
}

bool SettingsRegistry::parseFloat(const char* str, float& out) {
    char* end;
    float val = strtof(str, &end);
    if (end == str) return false;
    out = val;
    return true;
}

bool SettingsRegistry::parseInt(const char* str, int32_t& out) {
    char* end;
    long val = strtol(str, &end, 10);
    if (end == str) return false;
    out = (int32_t)val;
    return true;
}

bool SettingsRegistry::parseUint32(const char* str, uint32_t& out) {
    char* end;
    unsigned long val = strtoul(str, &end, 10);
    if (end == str) return false;
    out = (uint32_t)val;
    return true;
}

bool SettingsRegistry::parseBool(const char* str, bool& out) {
    if (strcasecmp(str, "true") == 0 || strcasecmp(str, "on") == 0 ||
        strcasecmp(str, "yes") == 0 || strcmp(str, "1") == 0) {
        out = true;
        return true;
    }
    if (strcasecmp(str, "false") == 0 || strcasecmp(str, "off") == 0 ||
        strcasecmp(str, "no") == 0 || strcmp(str, "0") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool SettingsRegistry::setValue(Setting* s, const char* valueStr) {
    float floatVal;
    int32_t intVal;
    bool boolVal;

    switch (s->type) {
        case SettingType::UINT8:
            if (!parseInt(valueStr, intVal)) return false;
            intVal = constrain(intVal, (int32_t)s->minVal, (int32_t)s->maxVal);
            *((uint8_t*)s->valuePtr) = (uint8_t)intVal;
            break;

        case SettingType::INT8:
            if (!parseInt(valueStr, intVal)) return false;
            intVal = constrain(intVal, (int32_t)s->minVal, (int32_t)s->maxVal);
            *((int8_t*)s->valuePtr) = (int8_t)intVal;
            break;

        case SettingType::UINT16:
            if (!parseInt(valueStr, intVal)) return false;
            intVal = constrain(intVal, (int32_t)s->minVal, (int32_t)s->maxVal);
            *((uint16_t*)s->valuePtr) = (uint16_t)intVal;
            break;

        case SettingType::UINT32: {
            uint32_t uintVal;
            if (!parseUint32(valueStr, uintVal)) return false;
            // Use manual min/max to avoid signed/unsigned issues
            uint32_t minU = (uint32_t)s->minVal;
            uint32_t maxU = (uint32_t)s->maxVal;
            if (uintVal < minU) uintVal = minU;
            if (uintVal > maxU) uintVal = maxU;
            *((uint32_t*)s->valuePtr) = uintVal;
            break;
        }

        case SettingType::FLOAT:
            if (!parseFloat(valueStr, floatVal)) return false;
            floatVal = constrain(floatVal, s->minVal, s->maxVal);
            *((float*)s->valuePtr) = floatVal;
            break;

        case SettingType::BOOL:
            if (!parseBool(valueStr, boolVal)) return false;
            *((bool*)s->valuePtr) = boolVal;
            break;

        default:
            return false;
    }

    // Call onChange callback if registered
    if (s->onChange) {
        s->onChange();
    }

    return true;
}

void SettingsRegistry::printSettingValue(const Setting& s) {
    Serial.print(s.name);
    Serial.print(F(" = "));

    switch (s.type) {
        case SettingType::UINT8:
            Serial.print(*((uint8_t*)s.valuePtr));
            break;
        case SettingType::INT8:
            Serial.print(*((int8_t*)s.valuePtr));
            break;
        case SettingType::UINT16:
            Serial.print(*((uint16_t*)s.valuePtr));
            break;
        case SettingType::UINT32:
            Serial.print(*((uint32_t*)s.valuePtr));
            break;
        case SettingType::FLOAT:
            Serial.print(*((float*)s.valuePtr), 3);
            break;
        case SettingType::BOOL:
            Serial.print(*((bool*)s.valuePtr) ? F("on") : F("off"));
            break;
    }

    Serial.print(F("  ["));
    Serial.print(s.category);
    Serial.println(F("]"));
}

void SettingsRegistry::printSettingHelp(const Setting& s) {
    Serial.print(F("  "));
    Serial.print(s.name);

    // Pad name to align descriptions
    int padding = 20 - strlen(s.name);
    while (padding-- > 0) Serial.print(' ');

    Serial.print(s.description);

    // Show range for numeric types
    if (s.type != SettingType::BOOL) {
        Serial.print(F(" ("));
        if (s.type == SettingType::FLOAT) {
            Serial.print(s.minVal, 1);
            Serial.print(F("-"));
            Serial.print(s.maxVal, 1);
        } else {
            Serial.print((int)s.minVal);
            Serial.print(F("-"));
            Serial.print((int)s.maxVal);
        }
        Serial.print(F(")"));
    } else {
        Serial.print(F(" (on/off)"));
    }
    Serial.println();
}

bool SettingsRegistry::handleCommand(const char* cmd) {
    // Skip empty commands
    if (!cmd || cmd[0] == '\0') return false;

    // Handle "set <name> <value>"
    if (strncmp(cmd, "set ", 4) == 0) {
        char nameBuf[32];
        char valueBuf[32];

        // Parse name and value
        const char* nameStart = cmd + 4;
        while (*nameStart == ' ') nameStart++;

        const char* valueStart = strchr(nameStart, ' ');
        if (!valueStart) {
            Serial.println(F("Usage: set <name> <value>"));
            return true;
        }

        // Copy name
        size_t nameLen = valueStart - nameStart;
        if (nameLen >= sizeof(nameBuf)) nameLen = sizeof(nameBuf) - 1;
        strncpy(nameBuf, nameStart, nameLen);
        nameBuf[nameLen] = '\0';

        // Copy value
        while (*valueStart == ' ') valueStart++;
        strncpy(valueBuf, valueStart, sizeof(valueBuf) - 1);
        valueBuf[sizeof(valueBuf) - 1] = '\0';

        // Find and set
        Setting* s = findSetting(nameBuf);
        if (!s) {
            Serial.print(F("Unknown setting: "));
            Serial.println(nameBuf);
            return true;
        }

        if (setValue(s, valueBuf)) {
            printSettingValue(*s);
        } else {
            Serial.print(F("Invalid value: "));
            Serial.println(valueBuf);
        }
        return true;
    }

    // Handle "get <name>"
    if (strncmp(cmd, "get ", 4) == 0) {
        const char* name = cmd + 4;
        while (*name == ' ') name++;

        Setting* s = findSetting(name);
        if (!s) {
            Serial.print(F("Unknown setting: "));
            Serial.println(name);
        } else {
            printSettingValue(*s);
        }
        return true;
    }

    // Handle "show" or "show <category>"
    if (strcmp(cmd, "show") == 0) {
        printAll();
        return true;
    }

    if (strncmp(cmd, "show ", 5) == 0) {
        const char* category = cmd + 5;
        while (*category == ' ') category++;
        printCategory(category);
        return true;
    }

    // Handle "list" - same as show
    if (strcmp(cmd, "list") == 0) {
        printAll();
        return true;
    }

    // Handle "categories"
    if (strcmp(cmd, "categories") == 0) {
        printCategories();
        return true;
    }

    // Handle "settings" or "settings help"
    if (strcmp(cmd, "settings") == 0 || strcmp(cmd, "settings help") == 0) {
        printHelp();
        return true;
    }

    return false;  // Command not handled
}

void SettingsRegistry::printValue(const char* name) {
    Setting* s = findSetting(name);
    if (s) {
        printSettingValue(*s);
    }
}

void SettingsRegistry::printAll() {
    Serial.println(F("=== ALL SETTINGS ==="));

    // Collect unique categories
    const char* categories[16];
    uint8_t numCategories = 0;

    for (uint8_t i = 0; i < numSettings_; i++) {
        const char* cat = settings_[i].category;
        bool found = false;
        for (uint8_t j = 0; j < numCategories; j++) {
            if (strcmp(categories[j], cat) == 0) {
                found = true;
                break;
            }
        }
        if (!found && numCategories < 16) {
            categories[numCategories++] = cat;
        }
    }

    // Print by category
    for (uint8_t c = 0; c < numCategories; c++) {
        Serial.println();
        Serial.print(F("["));
        Serial.print(categories[c]);
        Serial.println(F("]"));

        for (uint8_t i = 0; i < numSettings_; i++) {
            if (strcmp(settings_[i].category, categories[c]) == 0) {
                Serial.print(F("  "));
                printSettingValue(settings_[i]);
            }
        }
    }
}

void SettingsRegistry::printCategory(const char* category) {
    Serial.print(F("=== "));
    Serial.print(category);
    Serial.println(F(" SETTINGS ==="));

    bool found = false;
    for (uint8_t i = 0; i < numSettings_; i++) {
        if (strcasecmp(settings_[i].category, category) == 0) {
            printSettingValue(settings_[i]);
            found = true;
        }
    }

    if (!found) {
        Serial.print(F("No settings in category: "));
        Serial.println(category);
    }
}

void SettingsRegistry::printCategories() {
    Serial.println(F("=== CATEGORIES ==="));

    const char* categories[16];
    uint8_t counts[16] = {0};
    uint8_t numCategories = 0;

    for (uint8_t i = 0; i < numSettings_; i++) {
        const char* cat = settings_[i].category;
        bool found = false;
        for (uint8_t j = 0; j < numCategories; j++) {
            if (strcmp(categories[j], cat) == 0) {
                counts[j]++;
                found = true;
                break;
            }
        }
        if (!found && numCategories < 16) {
            categories[numCategories] = cat;
            counts[numCategories] = 1;
            numCategories++;
        }
    }

    for (uint8_t c = 0; c < numCategories; c++) {
        Serial.print(F("  "));
        Serial.print(categories[c]);
        Serial.print(F(" ("));
        Serial.print(counts[c]);
        Serial.println(F(" settings)"));
    }

    Serial.println();
    Serial.println(F("Use 'show <category>' to see settings in a category"));
}

void SettingsRegistry::printHelp() {
    Serial.println(F("=== SETTINGS COMMANDS ==="));
    Serial.println(F("  set <name> <value>  - Set a value"));
    Serial.println(F("  get <name>          - Get current value"));
    Serial.println(F("  show                - Show all settings"));
    Serial.println(F("  show <category>     - Show category settings"));
    Serial.println(F("  categories          - List all categories"));
    Serial.println();
    Serial.println(F("=== AVAILABLE SETTINGS ==="));

    // Collect unique categories
    const char* categories[16];
    uint8_t numCategories = 0;

    for (uint8_t i = 0; i < numSettings_; i++) {
        const char* cat = settings_[i].category;
        bool found = false;
        for (uint8_t j = 0; j < numCategories; j++) {
            if (strcmp(categories[j], cat) == 0) {
                found = true;
                break;
            }
        }
        if (!found && numCategories < 16) {
            categories[numCategories++] = cat;
        }
    }

    // Print by category with help text
    for (uint8_t c = 0; c < numCategories; c++) {
        Serial.println();
        Serial.print(F("["));
        Serial.print(categories[c]);
        Serial.println(F("]"));

        for (uint8_t i = 0; i < numSettings_; i++) {
            if (strcmp(settings_[i].category, categories[c]) == 0) {
                printSettingHelp(settings_[i]);
            }
        }
    }
}

const char* SettingsRegistry::typeString(SettingType t) {
    switch (t) {
        case SettingType::UINT8:  return "uint8";
        case SettingType::INT8:   return "int8";
        case SettingType::UINT16: return "uint16";
        case SettingType::UINT32: return "uint32";
        case SettingType::FLOAT:  return "float";
        case SettingType::BOOL:   return "bool";
        default:                  return "unknown";
    }
}

// Helper to print a single setting as JSON object (without leading comma)
void SettingsRegistry::printSettingJson(const Setting& s) {
    Serial.print(F("{\"name\":\""));
    Serial.print(s.name);
    Serial.print(F("\",\"value\":"));

    // Print value based on type
    switch (s.type) {
        case SettingType::UINT8:
            Serial.print(*((uint8_t*)s.valuePtr));
            break;
        case SettingType::INT8:
            Serial.print(*((int8_t*)s.valuePtr));
            break;
        case SettingType::UINT16:
            Serial.print(*((uint16_t*)s.valuePtr));
            break;
        case SettingType::UINT32:
            Serial.print(*((uint32_t*)s.valuePtr));
            break;
        case SettingType::FLOAT:
            Serial.print(*((float*)s.valuePtr), 3);
            break;
        case SettingType::BOOL:
            Serial.print(*((bool*)s.valuePtr) ? "true" : "false");
            break;
    }

    Serial.print(F(",\"type\":\""));
    Serial.print(typeString(s.type));
    Serial.print(F("\",\"cat\":\""));
    Serial.print(s.category);
    Serial.print(F("\",\"min\":"));

    // Print min/max based on type
    if (s.type == SettingType::FLOAT) {
        Serial.print(s.minVal, 3);
        Serial.print(F(",\"max\":"));
        Serial.print(s.maxVal, 3);
    } else {
        Serial.print((int32_t)s.minVal);
        Serial.print(F(",\"max\":"));
        Serial.print((int32_t)s.maxVal);
    }

    Serial.print(F(",\"desc\":\""));
    Serial.print(s.description);
    Serial.print(F("\"}"));
}

void SettingsRegistry::printSettingsJson() {
    Serial.print(F("{\"settings\":["));

    for (uint8_t i = 0; i < numSettings_; i++) {
        if (i > 0) Serial.print(',');
        printSettingJson(settings_[i]);
    }

    Serial.println(F("]}"));
}

void SettingsRegistry::printSettingsCategoryJson(const char* category) {
    Serial.print(F("{\"settings\":["));

    bool first = true;
    for (uint8_t i = 0; i < numSettings_; i++) {
        if (strcasecmp(settings_[i].category, category) != 0) continue;

        if (!first) Serial.print(',');
        first = false;

        printSettingJson(settings_[i]);
    }

    Serial.println(F("]}"));
}
