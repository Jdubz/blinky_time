#include "BleAdvertiser.h"

void BleAdvertiser::begin() {
    BLEDevice::init("Blinky-GW");
    advertising_ = BLEDevice::getAdvertising();
    ready_ = true;
    Serial.println(F("[BLE] Advertiser initialized"));
}

void BleAdvertiser::stop() {
    if (advertising_) {
        advertising_->stop();
    }
}

bool BleAdvertiser::broadcastSettings(const char* json) {
    return broadcast(BleProtocol::SETTINGS,
                     reinterpret_cast<const uint8_t*>(json), strlen(json));
}

bool BleAdvertiser::broadcastScene(const char* json) {
    return broadcast(BleProtocol::SCENE,
                     reinterpret_cast<const uint8_t*>(json), strlen(json));
}

bool BleAdvertiser::broadcastCommand(const char* cmd) {
    return broadcast(BleProtocol::COMMAND,
                     reinterpret_cast<const uint8_t*>(cmd), strlen(cmd));
}

bool BleAdvertiser::broadcast(BleProtocol::PacketType type,
                              const uint8_t* data, size_t len) {
    if (!ready_ || !advertising_) {
        errors_++;
        return false;
    }

    if (len > BleProtocol::MAX_PAYLOAD) {
        // TODO: fragmentation support for large payloads
        Serial.print(F("[BLE] Payload too large: "));
        Serial.print(len);
        Serial.print(F(" > "));
        Serial.println(BleProtocol::MAX_PAYLOAD);
        errors_++;
        return false;
    }

    buildAndSendPacket(type, data, len);
    return true;
}

void BleAdvertiser::buildAndSendPacket(BleProtocol::PacketType type,
                                       const uint8_t* payload, size_t payloadLen) {
    // Build manufacturer-specific data:
    // [company_id_lo, company_id_hi, header(4 bytes), payload...]
    // setManufacturerData expects the company ID as the first 2 bytes.
    size_t msdLen = 2 + BleProtocol::HEADER_SIZE + payloadLen;
    uint8_t msd[2 + BleProtocol::HEADER_SIZE + BleProtocol::MAX_PAYLOAD];

    // Company ID (little-endian)
    msd[0] = BleProtocol::COMPANY_ID & 0xFF;
    msd[1] = (BleProtocol::COMPANY_ID >> 8) & 0xFF;

    // Header
    BleProtocol::Header hdr;
    hdr.version = BleProtocol::PROTOCOL_VERSION;
    hdr.type = type;
    hdr.sequence = sequence_++;
    hdr.fragment = BleProtocol::FRAGMENT_SINGLE;
    memcpy(&msd[2], &hdr, sizeof(hdr));

    // Payload
    memcpy(&msd[2 + BleProtocol::HEADER_SIZE], payload, payloadLen);

    // Stop any current advertising before updating data
    advertising_->stop();

    // Set the advertising data
    BLEAdvertisementData advData;
    // setManufacturerData takes a String of raw bytes (includes company ID)
    advData.setManufacturerData(String(reinterpret_cast<char*>(msd), msdLen));

    advertising_->setAdvertisementData(advData);

    // Broadcast for a short burst (100ms at fast interval), then stop.
    // The advertising interval is set to minimum (20ms) for quick delivery.
    advertising_->setMinInterval(0x20);  // 20ms (32 * 0.625ms)
    advertising_->setMaxInterval(0x40);  // 40ms (64 * 0.625ms)

    // Start advertising — duration 0 means indefinite, we'll stop manually
    advertising_->start();

    // Advertise for 150ms (3-7 packets at 20-40ms interval) then stop.
    // This is enough for nearby scanners to pick up the packet.
    delay(150);
    advertising_->stop();

    packetsSent_++;
}

void BleAdvertiser::printDiagnostics() const {
    Serial.print(F("[BLE] role=advertiser state="));
    Serial.println(ready_ ? F("ready") : F("not_initialized"));

    Serial.print(F("[BLE] packets_tx="));
    Serial.print(packetsSent_);
    Serial.print(F(" errors="));
    Serial.println(errors_);

    Serial.print(F("[BLE] next_seq="));
    Serial.println(sequence_);
}
