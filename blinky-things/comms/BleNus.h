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

    /// Register DIS + NUS services. Does NOT start advertising — call
    /// startAdvertising() after every other GATT service is registered, so
    /// the attribute table is complete before any central can connect.
    /// Caller must have already called Bluefruit.begin(1, 0).
    void begin();

    /// Start advertising (NUS UUID, name, gossip-ACK scan response).
    void startAdvertising();

    /// Drain output ring buffer to BLEUart (one chunk per call).
    /// Call from main loop.
    void update();

    /// Register callback for complete lines received over NUS.
    void setLineCallback(LineCallback cb) { lineCallback_ = cb; }

    /// Send a line to the connected central (appends \n).
    void writeLine(const char* line);

    /// Re-encode the advertising payload with a new last-applied
    /// command_id (per BLE_FLEET_RELIABILITY_PLAN.md item #5 gossip
    /// ACK). The mfg-data block carries [company=0xFFFF][marker=0xA0
    /// ACK][cmd_id LE] and is embedded in the SCAN RESPONSE (not
    /// primary adv) to avoid disrupting NUS service discovery.
    ///
    /// Caller is expected to invoke this when ``BleScanner::
    /// getLastAcceptedCommandId()`` reports a value newer than the
    /// last advertised one. No-op if ``newId == lastAdvertisedCmdId_``.
    void setLastAckedCommandId(uint16_t newId);

    // --- Print interface (writes go into the ring buffer) ---
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* data, size_t size) override;

    /// Check if a central is connected.
    bool isConnected() const { return connected_; }

    /// Get connection handle.
    uint16_t getConnectionHandle() const { return connHandle_; }

    /// Negotiated ATT MTU of the current link, or 0 with no central connected.
    /// Reported in `ota status` so a web updater can size its BLE writes.
    // The link's usable ATT MTU: what the central negotiated, capped at the
    // SoftDevice's configured ceiling (MAX_ATT_MTU). BLEConnection::getMtu()
    // can report more than the SoftDevice uses: a Windows central asks for
    // 527 and getMtu() said 527 (hat_v1, 2026-10-21).
    uint16_t currentMtu() const;
    // configPrphBandwidth(BANDWIDTH_MAX) in setup(): the largest ATT MTU the
    // SoftDevice is configured for. Every notification is sized from this.
    static constexpr uint16_t MAX_ATT_MTU = 247;

    void printDiagnostics(Print& out) const;

private:
    static void onConnect(uint16_t conn_hdl);
    static void onDisconnect(uint16_t conn_hdl, uint8_t reason);
    static void onRxData(uint16_t conn_hdl);

    void processRxByte(char c);

    /// Send up to one MTU-chunk from the ring buffer to BLEUart.
    void drainTxBuffer();

    /// One notification on the NUS TX characteristic, or false at once when no HVN buffer
    /// is free (#189). Never waits: BLECharacteristic::notify() waits up to 100 ms for a
    /// buffer, on the loop task, and the record sender shares the same HVN queue.
    bool notifyNoWait(BLEConnection* conn, const uint8_t* data, uint16_t len);

    static BleNus* instance_;

    // RX FIFO sized for MTU-247 writes. The SoftDevice ceiling is now MTU
    // 247 (configPrphBandwidth), so the fleet server writes commands in
    // 244-byte chunks (it chunks at MTU-3). A `device upload` config is ~550
    // bytes = 3 chunks; the default 256-byte FIFO overflows (bytes dropped,
    // JSON corrupted) if the deferred RX callback falls one chunk behind.
    static const uint16_t RX_FIFO_DEPTH = 1024;
    // BLEUart with its TX characteristic's value handle exposed (protected in BLEUart),
    // for notifyNoWait.
    class Uart : public BLEUart {
    public:
        using BLEUart::BLEUart;
        uint16_t txValueHandle() { return _txd.handles().value_handle; }
    };
    Uart uart_{RX_FIFO_DEPTH};
    BLEDis dis_;

    LineCallback lineCallback_ = nullptr;

    // --- RX line assembly (Bluefruit callback task) ---
    static const size_t LINE_BUF_SIZE = 768;
    char lineBuf_[LINE_BUF_SIZE];
    size_t lineLen_ = 0;

    // --- Command queue: callback task -> loop task (#173) ---
    // Complete lines are queued here and executed by update() on the loop
    // task. Running handleCommand() in the Bluefruit callback task overran
    // its small stack on any long reply (`json info`, `ble`) and reset the
    // device (hat_v1, 2026-10-21). Single producer (callback task), single
    // consumer (loop task): lock-free, no long interrupt-off copies (lines
    // are up to 767 bytes; the SoftDevice tolerates ~100 us).
    static const uint8_t CMD_QUEUE_DEPTH = 4;
    char cmdQueue_[CMD_QUEUE_DEPTH][LINE_BUF_SIZE];
    volatile uint8_t cmdHead_ = 0;      // next slot the producer fills
    volatile uint8_t cmdTail_ = 0;      // next slot the consumer runs
    volatile uint32_t cmdDropped_ = 0;  // lines dropped: queue full (reported by update())
    uint32_t cmdDroppedReported_ = 0;
    void runQueuedCommands();

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
    uint32_t txBytesDropped_ = 0;  // Bytes lost to a full TX ring (monotonic)
    bool txDropLogged_ = false;

    // Last command_id we've encoded into the scan response. setLastAcked
    // CommandId() is a no-op when the new id matches this — saves the
    // (relatively expensive) Bluefruit ScanResponse.setData round-trip
    // when the main loop polls the scanner more often than commands
    // actually arrive.
    uint16_t lastAdvertisedCmdId_ = 0;
};
