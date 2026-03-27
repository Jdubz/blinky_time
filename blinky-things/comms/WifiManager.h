#pragma once

/**
 * WifiManager.h - WiFi credential storage and connection for ESP32-S3
 *
 * Stores SSID/password in NVS (Preferences). Provides serial commands
 * for configuration. Connection is manual (not auto-connect on boot)
 * to keep the MVP simple.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

class WifiManager {
public:
    void begin();

    // Credential management (stored in NVS flash)
    void setSsid(const char* ssid);
    void setPassword(const char* pass);
    void clearCredentials();
    bool hasCredentials() const;

    // Connection control
    bool connect();      // Connect using stored credentials
    void disconnect();

    // Status
    bool isConnected() const;
    void printStatus() const;

    // Getters for stored values
    const char* getSsid() const { return ssid_; }

private:
    void loadCredentials();
    void saveCredentials();

    Preferences prefs_;
    char ssid_[64];
    char password_[64];
    bool credentialsLoaded_ = false;

    static const char* NVS_NAMESPACE;
    static const char* NVS_KEY_SSID;
    static const char* NVS_KEY_PASS;
};
