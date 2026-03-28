#include "Esp32BleNus.h"

// NUS UUIDs (same as nRF52840 BleNus)
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// Static instance pointer for callbacks
static Esp32BleNus* s_instance = nullptr;

class NusServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
        if (s_instance) s_instance->onConnect(connInfo.getConnHandle());
    }
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
        if (s_instance) s_instance->onDisconnect(connInfo.getConnHandle());
        // Restart advertising after disconnect
        NimBLEDevice::startAdvertising();
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override {
        if (s_instance) s_instance->mtu_ = mtu - 3;  // ATT overhead
    }
};

class NusRxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override {
        if (s_instance) {
            NimBLEAttValue val = chr->getValue();
            s_instance->onRxData(val.data(), val.size());
        }
    }
};

static NusServerCallbacks serverCb;
static NusRxCallbacks rxCb;
static portMUX_TYPE s_txMux = portMUX_INITIALIZER_UNLOCKED;

void Esp32BleNus::begin() {
    s_instance = this;

    // Idempotent: safe to call if BleAdvertiser::begin() already initialized NimBLE
    if (!NimBLEDevice::isInitialized()) {
        NimBLEDevice::init("Blinky");
    }
    server_ = NimBLEDevice::createServer();
    server_->setCallbacks(&serverCb);

    // Create NUS service
    NimBLEService* service = server_->createService(NUS_SERVICE_UUID);

    // TX characteristic (device → central, notify)
    txChar_ = service->createCharacteristic(
        NUS_TX_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );

    // RX characteristic (central → device, write)
    rxChar_ = service->createCharacteristic(
        NUS_RX_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    rxChar_->setCallbacks(&rxCb);

    service->start();

    // Add NUS service UUID to advertising
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SERVICE_UUID);
    adv->setName("Blinky");
    adv->start();

    Serial.println(F("[BLE] NUS service started"));
}

void Esp32BleNus::update() {
    if (connected_) {
        drainTxBuffer();
    }
}

void Esp32BleNus::onConnect(uint16_t connId) {
    portENTER_CRITICAL(&s_txMux);
    connected_ = true;
    portEXIT_CRITICAL(&s_txMux);
    Serial.println(F("[BLE] NUS client connected"));
}

void Esp32BleNus::onDisconnect(uint16_t connId) {
    portENTER_CRITICAL(&s_txMux);
    connected_ = false;
    txHead_ = txTail_ = 0;  // Clear TX buffer
    portEXIT_CRITICAL(&s_txMux);
    Serial.println(F("[BLE] NUS client disconnected"));
}

void Esp32BleNus::onRxData(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (lineLen_ > 0) {
                lineBuf_[lineLen_] = '\0';
                linesRx_++;
                if (lineCallback_) {
                    lineCallback_(lineBuf_);
                }
                lineLen_ = 0;
            }
        } else if (lineLen_ < LINE_BUF_SIZE - 1) {
            lineBuf_[lineLen_++] = c;
        } else {
            Serial.println(F("[BLE] NUS RX line truncated"));
        }
    }
}

size_t Esp32BleNus::write(uint8_t c) {
    portENTER_CRITICAL(&s_txMux);
    size_t nextHead = (txHead_ + 1) % TX_BUF_SIZE;
    if (nextHead == txTail_) {
        portEXIT_CRITICAL(&s_txMux);
        return 0;  // Buffer full
    }
    txBuf_[txHead_] = c;
    txHead_ = nextHead;
    portEXIT_CRITICAL(&s_txMux);
    return 1;
}

size_t Esp32BleNus::write(const uint8_t* data, size_t size) {
    size_t written = 0;
    for (size_t i = 0; i < size; i++) {
        if (write(data[i]) == 0) break;
        written++;
    }
    return written;
}

void Esp32BleNus::drainTxBuffer() {
    if (!connected_ || !txChar_ || txHead_ == txTail_) return;

    portENTER_CRITICAL(&s_txMux);
    // Send up to one MTU-chunk per call
    size_t avail = (txHead_ >= txTail_)
        ? (txHead_ - txTail_)
        : (TX_BUF_SIZE - txTail_ + txHead_);
    size_t chunkSize = min(avail, (size_t)mtu_);

    uint8_t chunk[244];  // Max BLE ATT payload
    chunkSize = min(chunkSize, sizeof(chunk));

    for (size_t i = 0; i < chunkSize; i++) {
        chunk[i] = txBuf_[txTail_];
        txTail_ = (txTail_ + 1) % TX_BUF_SIZE;
    }
    portEXIT_CRITICAL(&s_txMux);

    txChar_->setValue(chunk, chunkSize);
    txChar_->notify();
}

void Esp32BleNus::printDiagnostics() const {
    Serial.print(F("[BLE] NUS connected="));
    Serial.print(connected_ ? F("yes") : F("no"));
    Serial.print(F(" mtu="));
    Serial.print(mtu_);
    Serial.print(F(" rx="));
    Serial.print(linesRx_);
    Serial.print(F(" tx_pending="));
    size_t pending = (txHead_ >= txTail_)
        ? (txHead_ - txTail_)
        : (TX_BUF_SIZE - txTail_ + txHead_);
    Serial.println(pending);
}
