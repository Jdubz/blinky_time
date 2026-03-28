#include "BleScanner.h"

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

void BleScanner::handleReport(ble_gap_evt_adv_report_t* report) {
    // Parse manufacturer-specific data from the advertising packet
    // The raw data blob is in report->data.p_data[0..report->data.len]
    // We need to find the MSD AD structure (type 0xFF).

    uint8_t* data = report->data.p_data;
    uint16_t len = report->data.len;
    uint16_t offset = 0;

    while (offset < len) {
        uint8_t adLen = data[offset];
        if (adLen == 0 || offset + adLen >= len) break;

        uint8_t adType = data[offset + 1];
        if (adType == 0xFF && adLen >= 2 + BleProtocol::HEADER_SIZE + 1) {
            // Manufacturer Specific Data found
            // adLen includes the type byte, so actual data starts at offset+2
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

            // Dedup: check sequence number against last from this source
            uint8_t* srcAddr = report->peer_addr.addr;
            bool sameSource = hasSource_ && memcmp(srcAddr, lastSourceAddr_, 6) == 0;

            if (sameSource && hdr.sequence == lastSequence_) {
                packetsDuped_++;
                return;  // Duplicate, ignore
            }

            // Update source tracking
            memcpy(lastSourceAddr_, srcAddr, 6);
            lastSequence_ = hdr.sequence;
            hasSource_ = true;
            lastRssi_ = report->rssi;

            // Extract payload
            size_t payloadOffset = 2 + BleProtocol::HEADER_SIZE;  // company ID + header
            size_t payloadLen = msdLen - payloadOffset;

            if (payloadLen == 0 || payloadLen >= RX_BUF_SIZE) {
                packetsDropped_++;
                return;
            }

            // Only queue if buffer is free (drop if previous not yet consumed)
            if (rxReady_) {
                packetsDropped_++;
                return;
            }

            memcpy((char*)rxBuffer_, &msd[payloadOffset], payloadLen);
            rxBuffer_[payloadLen] = '\0';
            rxLen_ = payloadLen;
            rxType_ = hdr.type;
            rxReady_ = true;
            packetsReceived_++;
            return;
        }

        offset += adLen + 1;
    }
}

void BleScanner::update() {
    if (!rxReady_) return;

    // Copy from volatile buffer
    char payload[RX_BUF_SIZE];
    size_t len = rxLen_;
    uint8_t type = rxType_;
    memcpy(payload, (const char*)rxBuffer_, len);
    payload[len] = '\0';
    rxReady_ = false;  // Free buffer for next packet

    // Route based on packet type
    if (callback_) {
        switch (type) {
            case BleProtocol::SETTINGS:
            case BleProtocol::SCENE:
            case BleProtocol::COMMAND:
                callback_(payload, len);
                break;
            default:
                packetsDropped_++;
                break;
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

    out.print(F("[BLE] last_seq="));
    out.print(lastSequence_);
    out.print(F(" uptime="));
    out.print(getUptimeMs() / 1000);
    out.println(F("s"));
}
