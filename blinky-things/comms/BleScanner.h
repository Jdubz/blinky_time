#pragma once

/**
 * BleScanner.h - Passive BLE advertising scanner for nRF52840
 *
 * Listens for BLE advertising packets from ESP32-S3 gateway devices
 * that match our protocol (company ID + version). Received commands
 * are queued and processed in the main loop via update().
 *
 * Uses Bluefruit52Lib (SoftDevice S140) for passive scanning.
 * Scanning is interrupt-driven — no main loop CPU cost except
 * when processing a received packet.
 */

#include <Arduino.h>
#include <bluefruit.h>
#include "BleProtocol.h"

class BleScanner {
public:
    using CommandCallback = void(*)(const char* payload, size_t len);

    void begin();
    void update();  // Call from main loop — processes queued packets

    void setCommandCallback(CommandCallback cb) { callback_ = cb; }

    // Diagnostics
    uint32_t getPacketsReceived() const { return packetsReceived_; }
    uint32_t getPacketsDuped() const { return packetsDuped_; }
    uint32_t getPacketsDropped() const { return packetsDropped_; }
    int8_t getLastRssi() const { return lastRssi_; }
    uint8_t getLastSequence() const { return lastSequence_; }
    bool isActive() const { return active_; }
    uint32_t getUptimeMs() const { return active_ ? (millis() - startMs_) : 0; }

    void printDiagnostics(Print& out) const;

private:
    static void scanCallback(ble_gap_evt_adv_report_t* report);
    void handleReport(ble_gap_evt_adv_report_t* report);

    static BleScanner* instance_;

    CommandCallback callback_ = nullptr;

    // Dedup: track last sequence per source.
    // For simplicity, just track the single most recent source.
    uint8_t lastSequence_ = 0xFF;
    uint8_t lastSourceAddr_[6] = {0};
    bool hasSource_ = false;

    // Diagnostics
    int8_t lastRssi_ = 0;
    uint32_t packetsReceived_ = 0;
    uint32_t packetsDuped_ = 0;
    uint32_t packetsDropped_ = 0;  // Buffer full / parse error
    uint32_t startMs_ = 0;
    bool active_ = false;

    // Receive buffer: filled in scan callback (ISR context), consumed in update()
    static const size_t RX_BUF_SIZE = 256;
    volatile char rxBuffer_[RX_BUF_SIZE];
    volatile size_t rxLen_ = 0;
    volatile uint8_t rxType_ = 0;
    volatile bool rxReady_ = false;
};
