#pragma once

/**
 * BleScanner.h - Passive BLE advertising scanner for nRF52840
 *
 * Listens for BLE advertising packets from the LemonCart broadcaster
 * that match our protocol (company ID + version). Received commands
 * are queued and processed in the main loop via update().
 *
 * Uses Bluefruit52Lib (SoftDevice S140) for passive scanning.
 * Scan reports are delivered via Bluefruit's deferred-callback
 * FreeRTOS task (TASK_PRIO_NORMAL) — which PREEMPTS loop_task
 * (TASK_PRIO_LOW). Brief noInterrupts() sections coordinate the
 * producer (scanCallback → handleReport) with the consumer
 * (update() in the main loop). See item #1 + #3 in
 * docs/BLE_FLEET_RELIABILITY_PLAN.md for the rationale behind the
 * multi-slot ring and per-(source, seq) dedup ring.
 */

#include <Arduino.h>
#include <bluefruit.h>
#include "BleProtocol.h"

class BleScanner {
public:
    using CommandCallback = void(*)(const char* payload, size_t len);

    // Ring sizes — placed in the header so changes are visible to
    // anyone reading the class shape. Memory budget at the current
    // values: 8 × (1 + 2 + 240) = ~1.95 KB for rxRing_; 8 × 7 = 56 B
    // for seenRing_. Fine for nRF52840 (256 KB SRAM).
    //
    // RX_RING_SIZE 8 covers a 5× re-emit burst plus one back-to-back
    // command while the main loop is stalled in audio-NN inference.
    // The current single-slot design loses any second packet that
    // arrives before update() drains, which is the dominant cause of
    // dropped fleet commands (BLE_FLEET_RELIABILITY_PLAN.md item #1).
    static const size_t RX_RING_SIZE = 8;

    // SEEN_RING_SIZE 8 closes the false-dedup tail risk from a
    // single-source dedup memory: if two broadcasters interleave (the
    // LemonCart cart plus, say, a neighbouring cart at a festival
    // deployment), the single-source memory would treat each new
    // source's first packet as "fresh-seq from same source" and might
    // dedup mismatches. Tracking the last 8 (source, seq) tuples
    // accommodates ~3 distinct broadcasters with a healthy seq
    // history (BLE_FLEET_RELIABILITY_PLAN.md item #3).
    static const size_t SEEN_RING_SIZE = 8;

    // Matches BleProtocol::MAX_PAYLOAD. Kept as its own constant so the
    // slot layout below is self-describing.
    static const size_t SLOT_PAYLOAD_MAX = BleProtocol::MAX_PAYLOAD;

    void begin();
    void update();  // Call from main loop — drains all ready slots

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

    // Producer-only helpers — touch seenRing_ which is producer-only,
    // so no synchronization is involved. Naming reflects that.
    bool isDuplicateSeen(const uint8_t* srcAddr, uint8_t seq) const;
    void recordSeen(const uint8_t* srcAddr, uint8_t seq);

    static BleScanner* instance_;

    CommandCallback callback_ = nullptr;

    // Multi-slot receive ring. Producer = scanCallback (high-prio task),
    // consumer = update() (low-prio loop). Drop-oldest on overrun: when
    // a fresh packet arrives and the ring is full, the oldest slot is
    // discarded so the newest operator command always lands. Mutation
    // of (rxHead_, rxTail_) happens under noInterrupts() in both
    // producer and consumer.
    struct RxSlot {
        uint8_t type;
        uint16_t len;
        uint8_t data[SLOT_PAYLOAD_MAX + 1];  // +1 for nul terminator
    };
    RxSlot rxRing_[RX_RING_SIZE];
    volatile uint8_t rxHead_ = 0;
    volatile uint8_t rxTail_ = 0;

    // Per-(source, seq) dedup ring. Only touched by the producer — no
    // synchronization needed. FIFO eviction.
    struct SeenKey {
        uint8_t addr[6];
        uint8_t seq;
    };
    SeenKey seenRing_[SEEN_RING_SIZE];
    uint8_t seenCount_ = 0;
    uint8_t seenHead_ = 0;

    // Diagnostics — incremented in producer context; main-loop reads
    // are best-effort snapshots (a single byte-aligned uint32 read is
    // atomic enough on Cortex-M4 for diag-printing purposes).
    uint8_t lastSequence_ = 0xFF;
    int8_t lastRssi_ = 0;
    uint32_t packetsReceived_ = 0;
    uint32_t packetsDuped_ = 0;
    uint32_t packetsDropped_ = 0;
    uint32_t startMs_ = 0;
    bool active_ = false;
};
