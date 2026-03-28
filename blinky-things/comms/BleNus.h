#pragma once

/**
 * BleNus.h - Nordic UART Service (NUS) peripheral for nRF52840
 *
 * Provides bidirectional serial-over-BLE communication using the standard
 * Nordic UART Service (6E400001-B5A3-F393-E0A9-E50E24DCCA9E).
 *
 * The Pi fleet server connects via bleak and exchanges text commands
 * using the same line-based protocol as USB serial.
 *
 * Coexists with BleScanner — both use the shared SoftDevice.
 * Caller must call Bluefruit.begin(1, 0) before begin().
 */

#include <Arduino.h>
#include <bluefruit.h>

/// BLE NUS peripheral with paced output.
///
/// Inherits from Print so it can be used as TeeStream's secondary output.
/// Output goes into a ring buffer and is drained in update() at a rate
/// the BLE stack can handle (one MTU-chunk per call, ~1 notification per
/// main-loop iteration).  This avoids overwhelming the SoftDevice's
/// limited HVN TX queue, which causes data loss at small MTU sizes.
class BleNus : public Print {
public:
    using LineCallback = void(*)(const char* line);

    /// Initialize NUS service and start advertising.
    /// Caller must have already called Bluefruit.begin(1, 0).
    void begin();

    /// Drain output ring buffer to BLEUart (one chunk per call).
    /// Call from main loop.
    void update();

    /// Register callback for complete lines received over NUS.
    void setLineCallback(LineCallback cb) { lineCallback_ = cb; }

    /// Send a line to the connected central (appends \n).
    void writeLine(const char* line);

    // --- Print interface (writes go into the ring buffer) ---
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* data, size_t size) override;

    /// Check if a central is connected.
    bool isConnected() const { return connected_; }

    /// Get connection handle.
    uint16_t getConnectionHandle() const { return connHandle_; }

    void printDiagnostics(Print& out) const;

private:
    static void onConnect(uint16_t conn_hdl);
    static void onDisconnect(uint16_t conn_hdl, uint8_t reason);
    static void onRxData(uint16_t conn_hdl);

    void startAdvertising();
    void processRxByte(char c);

    /// Send up to one MTU-chunk from the ring buffer to BLEUart.
    void drainTxBuffer();

    static BleNus* instance_;

    BLEUart uart_;
    BLEDis dis_;

    LineCallback lineCallback_ = nullptr;

    // --- RX line assembly ---
    static const size_t LINE_BUF_SIZE = 768;
    char lineBuf_[LINE_BUF_SIZE];
    size_t lineLen_ = 0;

    // --- TX ring buffer (paced output to BLEUart) ---
    static const size_t TX_BUF_SIZE = 4096;
    uint8_t txBuf_[TX_BUF_SIZE];
    volatile size_t txHead_ = 0;  // write position
    volatile size_t txTail_ = 0;  // read position

    uint16_t connHandle_ = BLE_CONN_HANDLE_INVALID;
    bool connected_ = false;
    uint32_t connectTimeMs_ = 0;
    uint32_t linesRx_ = 0;
    uint32_t linesTx_ = 0;
};
