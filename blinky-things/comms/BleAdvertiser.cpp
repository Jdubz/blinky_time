#include "BleAdvertiser.h"

void BleAdvertiser::begin() {
    NimBLEDevice::init("Blinky-GW");
    advertising_ = NimBLEDevice::getAdvertising();
    ready_ = true;
    Serial.println(F("[BLE] Advertiser initialized (NimBLE)"));
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

    // Skip broadcast if a NUS client is connected — stopping advertising
    // would disrupt the active GATT connection (shared NimBLE advertising instance).
    if (NimBLEDevice::getServer() && NimBLEDevice::getServer()->getConnectedCount() > 0) {
        return false;
    }

    if (len > BleProtocol::MAX_PAYLOAD) {
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

    // Stop current advertising before updating data
    advertising_->stop();

    // Set manufacturer data (NimBLE takes raw uint8_t* + length)
    NimBLEAdvertisementData advData;
    advData.setManufacturerData(std::string(reinterpret_cast<char*>(msd), msdLen));
    advertising_->setAdvertisementData(advData);

    // Broadcast at fast interval for quick delivery
    advertising_->setMinInterval(0x20);  // 20ms
    advertising_->setMaxInterval(0x40);  // 40ms

    advertising_->start();
    // Blocking: NimBLE needs time to send 3-7 packets at the 20-40ms advertising
    // interval configured above. Without this delay, stop() cancels before any
    // packets are actually transmitted.
    delay(150);
    advertising_->stop();

    packetsSent_++;
}

void BleAdvertiser::printDiagnostics(Print& out) const {
    out.print(F("[BLE] role=advertiser state="));
    out.println(ready_ ? F("ready") : F("not_initialized"));
    out.print(F("[BLE] packets_tx="));
    out.print(packetsSent_);
    out.print(F(" errors="));
    out.println(errors_);
    out.print(F("[BLE] next_seq="));
    out.println(sequence_);
}
