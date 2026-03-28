#pragma once

/**
 * Esp32BleNus.h - Nordic UART Service (NUS) for ESP32-S3 using NimBLE
 *
 * Same protocol as the nRF52840 BleNus, but using NimBLE-Arduino 2.x API.
 * Provides bidirectional serial-over-BLE for fleet server communication.
 *
 * NUS UUIDs:
 *   Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX:      6E400002 (central writes commands here)
 *   TX:      6E400003 (device notifies responses here)
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

class Esp32BleNus : public Print {
public:
    using LineCallback = void(*)(const char* line);

    /// Initialize NUS server and start advertising.
    /// Call after NimBLEDevice::init().
    void begin();

    /// Drain TX buffer (call from loop).
    void update();

    /// Register callback for received lines.
    void setLineCallback(LineCallback cb) { lineCallback_ = cb; }

    // Print interface (buffered, drained in update())
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* data, size_t size) override;

    bool isConnected() const { return connected_; }
    void printDiagnostics() const;

private:
    friend class NusServerCallbacks;
    friend class NusRxCallbacks;

    void onConnect(uint16_t connId);
    void onDisconnect(uint16_t connId);
    void onRxData(const uint8_t* data, size_t len);
    void drainTxBuffer();

    NimBLEServer* server_ = nullptr;
    NimBLECharacteristic* txChar_ = nullptr;  // Notify (device → central)
    NimBLECharacteristic* rxChar_ = nullptr;  // Write (central → device)

    LineCallback lineCallback_ = nullptr;

    // RX line assembly
    static const size_t LINE_BUF_SIZE = 768;
    char lineBuf_[LINE_BUF_SIZE];
    size_t lineLen_ = 0;

    // TX ring buffer
    static const size_t TX_BUF_SIZE = 4096;
    uint8_t txBuf_[TX_BUF_SIZE];
    volatile size_t txHead_ = 0;
    volatile size_t txTail_ = 0;

    bool connected_ = false;
    uint16_t mtu_ = 20;
    uint32_t linesRx_ = 0;
};
