#include "WifiManager.h"

const char* WifiManager::NVS_NAMESPACE = "blinky_wifi";
const char* WifiManager::NVS_KEY_SSID  = "ssid";
const char* WifiManager::NVS_KEY_PASS  = "pass";

void WifiManager::begin() {
    memset(ssid_, 0, sizeof(ssid_));
    memset(password_, 0, sizeof(password_));
    loadCredentials();
    Serial.print(F("[WiFi] Manager ready"));
    if (hasCredentials()) {
        Serial.print(F(", stored SSID: "));
        Serial.print(ssid_);
    }
    Serial.println();
}

void WifiManager::loadCredentials() {
    prefs_.begin(NVS_NAMESPACE, true);  // read-only
    size_t ssidLen = prefs_.getString(NVS_KEY_SSID, ssid_, sizeof(ssid_));
    prefs_.getString(NVS_KEY_PASS, password_, sizeof(password_));
    prefs_.end();
    credentialsLoaded_ = (ssidLen > 0);
}

void WifiManager::saveCredentials() {
    prefs_.begin(NVS_NAMESPACE, false);  // read-write
    prefs_.putString(NVS_KEY_SSID, ssid_);
    prefs_.putString(NVS_KEY_PASS, password_);
    prefs_.end();
    credentialsLoaded_ = true;
}

void WifiManager::setSsid(const char* ssid) {
    strncpy(ssid_, ssid, sizeof(ssid_) - 1);
    ssid_[sizeof(ssid_) - 1] = '\0';
    saveCredentials();
    Serial.print(F("[WiFi] SSID set: "));
    Serial.println(ssid_);
}

void WifiManager::setPassword(const char* pass) {
    strncpy(password_, pass, sizeof(password_) - 1);
    password_[sizeof(password_) - 1] = '\0';
    saveCredentials();
    Serial.println(F("[WiFi] Password set"));
}

void WifiManager::clearCredentials() {
    prefs_.begin(NVS_NAMESPACE, false);
    prefs_.clear();
    prefs_.end();
    memset(ssid_, 0, sizeof(ssid_));
    memset(password_, 0, sizeof(password_));
    credentialsLoaded_ = false;
    Serial.println(F("[WiFi] Credentials cleared"));
}

bool WifiManager::hasCredentials() const {
    return credentialsLoaded_ && ssid_[0] != '\0';
}

bool WifiManager::connect() {
    if (!hasCredentials()) {
        Serial.println(F("[WiFi] No credentials stored. Use: wifi ssid <name>, wifi pass <key>"));
        return false;
    }

    Serial.print(F("[WiFi] Connecting to "));
    Serial.print(ssid_);
    Serial.print(F("..."));

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid_, password_);

    // Wait up to 10 seconds for connection
    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 10000) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        // Disable WiFi power management — prevents the radio from sleeping
        // and dropping TCP connections during idle periods.
        WiFi.setSleep(false);
        Serial.print(F("[WiFi] Connected! IP: "));
        Serial.print(WiFi.localIP());
        Serial.print(F(" RSSI: "));
        Serial.print(WiFi.RSSI());
        Serial.println(F("dBm"));
        return true;
    } else {
        Serial.print(F("[WiFi] Connection failed, status="));
        Serial.println(WiFi.status());
        return false;
    }
}

void WifiManager::disconnect() {
    WiFi.disconnect(true);  // true = turn off WiFi radio
    Serial.println(F("[WiFi] Disconnected"));
}

bool WifiManager::isConnected() const {
    return WiFi.status() == WL_CONNECTED;
}

void WifiManager::printStatus() const {
    Serial.print(F("[WiFi] status="));
    if (isConnected()) {
        Serial.print(F("connected ssid="));
        Serial.print(WiFi.SSID());
        Serial.print(F(" ip="));
        Serial.print(WiFi.localIP());
        Serial.print(F(" rssi="));
        Serial.print(WiFi.RSSI());
        Serial.println(F("dBm"));
    } else {
        Serial.print(F("disconnected"));
        if (hasCredentials()) {
            Serial.print(F(" (stored ssid: "));
            Serial.print(ssid_);
            Serial.print(F(")"));
        }
        Serial.println();
    }
}
