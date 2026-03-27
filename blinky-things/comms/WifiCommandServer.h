#pragma once

/**
 * WifiCommandServer — Non-blocking TCP server for wireless fleet management.
 *
 * ALL operations run on Core 1 (main loop), fully non-blocking:
 *   - WiFi.begin() is blocking only once in setup() (up to 10s)
 *   - server.available() returns immediately (non-blocking accept)
 *   - client.available() + client.read() are non-blocking
 *
 * ESP32 lwIP is NOT thread-safe across cores. WiFi.status(), server.available(),
 * and client read/write must all run on the same core. Since setup() and loop()
 * run on Core 1, everything stays there.
 *
 * Port: 3333 (default). Same line-based protocol as serial and BLE NUS.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>

class SerialConsole;

class WifiCommandServer : public Print {
public:
    static constexpr uint16_t DEFAULT_PORT = 3333;
    static constexpr size_t CMD_MAX_LEN = 512;

    explicit WifiCommandServer(uint16_t port = DEFAULT_PORT);

    /// Start TCP server. Call after WiFi is connected.
    void begin();

    /// Poll for new clients and incoming data. Call from loop().
    /// Returns true if a command was processed.
    bool poll();

    void setConsole(SerialConsole* console) { console_ = console; }

    // Print interface — writes go to TCP client
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t size) override;

    // Status
    bool isWifiConnected() const { return WiFi.status() == WL_CONNECTED; }
    bool hasClient() const { return clientConnected_; }
    uint16_t getPort() const { return port_; }
    uint32_t getLinesReceived() const { return linesRx_; }
    IPAddress getIP() const { return WiFi.localIP(); }

    void printDiagnostics();
    void printDiagnostics(Print& out);

private:
    uint16_t port_;
    WiFiServer server_;
    WiFiClient client_;
    bool serverStarted_ = false;
    bool clientConnected_ = false;

    char rxBuf_[CMD_MAX_LEN];
    size_t rxPos_ = 0;

    SerialConsole* console_ = nullptr;

    // Stats
    uint32_t linesRx_ = 0;
    uint32_t connectCount_ = 0;
};
