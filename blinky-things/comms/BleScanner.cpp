#include "BleScanner.h"
#include <string.h>

BleScanner* BleScanner::instance_ = nullptr;

void BleScanner::begin() {
    instance_ = this;

    // NOTE: Bluefruit.begin() must be called by main sketch BEFORE this method.
    // When NUS peripheral is also active, main sketch calls Bluefruit.begin(1, 0).
    // When scanner-only, main sketch calls Bluefruit.begin(0, 0).
    // Bluefruit.setName() and setTxPower() are also set by main sketch.

    // Configure scanner
    Bluefruit.Scanner.setRxCallback(scanCallback);
    Bluefruit.Scanner.restartOnDisconnect(true);

    // Passive scanning — we only read advertising data, no scan requests sent.
    // Minimizes power and RF interference.
    Bluefruit.Scanner.useActiveScan(false);

    // Scan interval/window: 160/80 (100ms interval, 50ms window = 50% duty)
    // Good balance of responsiveness vs power. SoftDevice handles timing via ISR.
    Bluefruit.Scanner.setInterval(160, 80);  // Units of 0.625ms

    // Filter: only report packets containing manufacturer-specific data
    // This reduces callback frequency significantly (ignores iBeacons, etc.)
    Bluefruit.Scanner.filterMSD(BleProtocol::COMPANY_ID);

    // Start scanning (0 = scan indefinitely)
    Bluefruit.Scanner.start(0);

    startMs_ = millis();
    active_ = true;

    Serial.println(F("[BLE] Scanner started (passive, filtering company 0xFFFF)"));
}

void BleScanner::scanCallback(ble_gap_evt_adv_report_t* report) {
    if (!instance_) return;
    instance_->handleReport(report);

    // IMPORTANT: Resume scanning after processing this report.
    // Bluefruit pauses scanning after each callback to prevent buffer overflows.
    Bluefruit.Scanner.resume();
}

bool BleScanner::isDuplicateSeen(const uint8_t* srcAddr, uint8_t seq) const {
    // Linear scan over the seen ring. SEEN_RING_SIZE is small (8) and
    // this runs from a NORMAL-prio FreeRTOS task — well within budget.
    for (size_t i = 0; i < seenCount_; ++i) {
        const SeenKey& k = seenRing_[i];
        if (k.seq == seq && memcmp(k.addr, srcAddr, 6) == 0) {
            return true;
        }
    }
    return false;
}

void BleScanner::recordSeen(const uint8_t* srcAddr, uint8_t seq) {
    // FIFO insert. When count < ring size, append; otherwise overwrite
    // the oldest entry (at seenHead_) and advance head. After
    // saturation, seenHead_ marks the oldest entry; entries are stored
    // in insertion order modulo SEEN_RING_SIZE.
    SeenKey& slot = seenRing_[seenHead_];
    memcpy(slot.addr, srcAddr, 6);
    slot.seq = seq;
    seenHead_ = (seenHead_ + 1) % SEEN_RING_SIZE;
    if (seenCount_ < SEEN_RING_SIZE) {
        ++seenCount_;
    }
}

void BleScanner::handleReport(ble_gap_evt_adv_report_t* report) {
    // Parse manufacturer-specific data from the advertising packet.
    // The raw data blob is in report->data.p_data[0..report->data.len]
    // We need to find the MSD AD structure (type 0xFF).

    uint8_t* data = report->data.p_data;
    uint16_t len = report->data.len;
    uint16_t offset = 0;

    while (offset < len) {
        uint8_t adLen = data[offset];
        if (adLen == 0 || offset + adLen >= len) break;

        uint8_t adType = data[offset + 1];
        if (adType != 0xFF || adLen < 2 + BleProtocol::HEADER_SIZE + 1) {
            offset += adLen + 1;
            continue;
        }

        // Manufacturer Specific Data found.
        // adLen includes the type byte, so the MSD body starts at offset+2.
        uint8_t* msd = &data[offset + 2];
        uint8_t msdLen = adLen - 1;  // Subtract type byte

        // Check company ID (little-endian)
        uint16_t companyId = msd[0] | (msd[1] << 8);
        if (companyId != BleProtocol::COMPANY_ID) {
            offset += adLen + 1;
            continue;
        }

        // Parse header (after 2-byte company ID)
        if (msdLen < 2 + BleProtocol::HEADER_SIZE) {
            offset += adLen + 1;
            continue;
        }

        BleProtocol::Header hdr;
        memcpy(&hdr, &msd[2], sizeof(hdr));

        // Version check
        if (hdr.version != BleProtocol::PROTOCOL_VERSION) {
            offset += adLen + 1;
            continue;
        }

        // Per-(source, seq) dedup against the seen ring. Multiple emits
        // of the SAME (source, seq) tuple — which BlueZ produces when
        // its advertising interval retransmits a single payload — get
        // collapsed to one accepted packet. Re-emits with FRESH seqs
        // (the server's COMMAND_REEMIT_COUNT path) sail through.
        const uint8_t* srcAddr = report->peer_addr.addr;
        if (isDuplicateSeen(srcAddr, hdr.sequence)) {
            ++packetsDuped_;
            return;
        }

        // Validate payload bounds BEFORE recording in the seen ring.
        // Otherwise a malformed-size packet would consume a slot and
        // could displace a legitimate older (src, seq) on FIFO eviction.
        size_t payloadOffset = 2 + BleProtocol::HEADER_SIZE;  // company ID + header
        size_t payloadLen = msdLen - payloadOffset;
        if (payloadLen == 0 || payloadLen > SLOT_PAYLOAD_MAX) {
            ++packetsDropped_;
            return;
        }

        recordSeen(srcAddr, hdr.sequence);
        lastSequence_ = hdr.sequence;
        lastRssi_ = report->rssi;

        // Enqueue into the rx ring with drop-oldest on overrun. The
        // index updates + slot write share a brief noInterrupts()
        // window with the consumer's slot-copy + tail-advance in
        // update(), so a partial overwrite can never race a slot read.
        noInterrupts();
        uint8_t head = rxHead_;
        uint8_t tail = rxTail_;
        uint8_t nextHead = (head + 1) % RX_RING_SIZE;
        bool full = (nextHead == tail);
        if (full) {
            // Drop oldest — preserves the newest operator command at the
            // cost of one already-queued packet. Idempotency at the
            // command level (gen / effect / set / save / load) means a
            // dropped intermediate is safe to replay if it ever matters.
            rxTail_ = (tail + 1) % RX_RING_SIZE;
            ++packetsDropped_;
        }
        RxSlot& slot = rxRing_[head];
        slot.type = hdr.type;
        slot.len = static_cast<uint16_t>(payloadLen);
        memcpy(slot.data, &msd[payloadOffset], payloadLen);
        slot.data[payloadLen] = '\0';
        rxHead_ = nextHead;
        ++packetsReceived_;
        interrupts();

        return;
    }
}

void BleScanner::update() {
    // Drain every ready slot per call. Without this, a back-to-back
    // command burst (5× re-emit + queued follow-on) leaves slots
    // stranded until the next main-loop tick, defeating the ring's
    // purpose. Bounded to RX_RING_SIZE * 2 iterations so a producer
    // that keeps refilling during drain can't starve the main loop;
    // any leftover slots get picked up on the next loop tick.
    for (size_t iter = 0; iter < RX_RING_SIZE * 2; ++iter) {
        char payload[SLOT_PAYLOAD_MAX + 1];
        uint8_t type;
        size_t len;

        // Snapshot + copy + advance tail under noInterrupts() so the
        // producer's drop-oldest path cannot overwrite the slot mid-copy.
        // The copy is at most SLOT_PAYLOAD_MAX bytes (~240) — well
        // under the SoftDevice's tolerated PRIMASK window.
        noInterrupts();
        if (rxHead_ == rxTail_) {
            interrupts();
            return;
        }
        uint8_t tail = rxTail_;
        const RxSlot& slot = rxRing_[tail];
        type = slot.type;
        len = slot.len;
        // Producer's bounds check in handleReport() guarantees
        // 0 < len <= SLOT_PAYLOAD_MAX. No re-clamp here.
        memcpy(payload, slot.data, len);
        payload[len] = '\0';
        rxTail_ = (tail + 1) % RX_RING_SIZE;
        interrupts();

        // Dispatch outside the critical section.
        if (callback_) {
            switch (type) {
                case BleProtocol::SETTINGS:
                case BleProtocol::SCENE:
                case BleProtocol::COMMAND:
                    callback_(payload, len);
                    break;
                default:
                    // Unknown packet type (e.g. the server's 0x00 no-op
                    // terminator). Count as a drop so it's visible in
                    // diagnostics rather than silently absorbed.
                    ++packetsDropped_;
                    break;
            }
        }
    }
}

void BleScanner::printDiagnostics(Print& out) const {
    out.print(F("[BLE] role=scanner state="));
    out.println(active_ ? F("active") : F("inactive"));

    out.print(F("[BLE] packets_rx="));
    out.print(packetsReceived_);
    out.print(F(" duped="));
    out.print(packetsDuped_);
    out.print(F(" dropped="));
    out.print(packetsDropped_);
    if (packetsReceived_ > 0) {
        out.print(F(" last_rssi="));
        out.print(lastRssi_);
        out.print(F("dBm"));
    }
    out.println();

    // Ring depth at print time — useful for spotting a stalled consumer
    // (main loop hanging in NN inference while packets queue up).
    uint8_t head = rxHead_;
    uint8_t tail = rxTail_;
    uint8_t depth = (head + RX_RING_SIZE - tail) % RX_RING_SIZE;
    out.print(F("[BLE] rx_ring="));
    out.print(depth);
    out.print(F("/"));
    out.print(RX_RING_SIZE);
    out.print(F(" seen_ring="));
    out.print(seenCount_);
    out.print(F("/"));
    out.println(SEEN_RING_SIZE);

    out.print(F("[BLE] last_seq="));
    out.print(lastSequence_);
    out.print(F(" uptime="));
    out.print(getUptimeMs() / 1000);
    out.println(F("s"));
}
