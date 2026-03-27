#pragma once

/**
 * WifiCommandServer — TCP server for wireless device management (ESP32-S3).
 *
 * Listens on a configurable TCP port (default 3333). Accepts one client at a
 * time. Incoming lines are routed to SerialConsole::handleCommand(). Responses
 * are sent back over TCP via a Print adapter.
 *
 * Same line-based protocol as serial and BLE NUS — the fleet server
 * (blinky-server) uses WifiTransport to connect.
 *
 * Call update() from the main loop to accept connections and process data.
 * Non-blocking — uses WiFiServer.available() and WiFiClient.available().
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>

class SerialConsole;  // Forward declaration

class WifiCommandServer : public Print {
public:
    static constexpr uint16_t DEFAULT_PORT = 3333;
    static constexpr size_t RX_BUF_SIZE = 512;

    explicit WifiCommandServer(uint16_t port = DEFAULT_PORT);

    void begin();
    void update();  // Call from main loop — non-blocking

    // Set the console that will handle incoming commands
    void setConsole(SerialConsole* console) { console_ = console; }

    // Print interface — responses written here go to the TCP client
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t size) override;

    // Status
    bool hasClient() { return client_ && client_.connected(); }
    uint16_t getPort() const { return port_; }
    uint32_t getLinesReceived() const { return linesRx_; }
    uint32_t getLinesSent() const { return linesTx_; }

    void printDiagnostics();

private:
    void processRxByte(uint8_t b);

    WiFiServer server_;
    WiFiClient client_;
    SerialConsole* console_ = nullptr;
    uint16_t port_;
    bool started_ = false;

    // RX line assembly
    char rxBuf_[RX_BUF_SIZE];
    size_t rxPos_ = 0;

    // Stats
    uint32_t linesRx_ = 0;
    uint32_t linesTx_ = 0;
    uint32_t connectCount_ = 0;
};
