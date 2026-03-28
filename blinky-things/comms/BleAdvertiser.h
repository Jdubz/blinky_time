#pragma once

/**
 * BleAdvertiser.h - BLE advertising broadcaster for ESP32-S3
 *
 * Broadcasts settings/commands as BLE advertising packets with
 * manufacturer-specific data. nRF52840 devices passively scan
 * and apply matching packets.
 *
 * Uses NimBLE-Arduino library (v2.3.8+ required for ESP32-S3).
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "BleProtocol.h"

class BleAdvertiser {
public:
    void begin();
    void stop();

    // Broadcast a payload to all scanning devices.
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

    NimBLEAdvertising* advertising_ = nullptr;
    uint8_t sequence_ = 0;
    uint32_t packetsSent_ = 0;
    uint32_t errors_ = 0;
    bool ready_ = false;
};
