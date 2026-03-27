#pragma once

/**
 * BleAdvertiser.h - BLE advertising broadcaster for ESP32-S3
 *
 * Broadcasts settings/commands as BLE advertising packets with
 * manufacturer-specific data. nRF52840 devices passively scan
 * and apply matching packets.
 *
 * Uses ESP32 BLE library (NimBLE or Bluedroid backend).
 * Advertising runs on Core 0 by default — no impact on Core 1 render loop.
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include "BleProtocol.h"

class BleAdvertiser {
public:
    void begin();
    void stop();

    // Broadcast a payload to all scanning devices.
    // Returns true if the packet was sent successfully.
    bool broadcastSettings(const char* json);
    bool broadcastScene(const char* json);
    bool broadcastCommand(const char* cmd);

    // Diagnostics
    uint32_t getPacketsSent() const { return packetsSent_; }
    uint32_t getErrors() const { return errors_; }
    bool isReady() const { return ready_; }
    void printDiagnostics() const;

private:
    bool broadcast(BleProtocol::PacketType type, const uint8_t* data, size_t len);
    void buildAndSendPacket(BleProtocol::PacketType type,
                            const uint8_t* payload, size_t payloadLen);

    BLEAdvertising* advertising_ = nullptr;
    uint8_t sequence_ = 0;
    uint32_t packetsSent_ = 0;
    uint32_t errors_ = 0;
    bool ready_ = false;
};
