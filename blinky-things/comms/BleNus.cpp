#include "BleNus.h"

BleNus* BleNus::instance_ = nullptr;

void BleNus::begin() {
    instance_ = this;

    // Bluefruit.begin(1, 0) must already have been called by main sketch.
    // Bluefruit.setName() and setTxPower() are also set by main sketch.

    // Device Information Service
    dis_.setManufacturer("Blinky Time");
    dis_.setModel("nRF52840");
    dis_.begin();

    // Nordic UART Service
    uart_.begin();
    uart_.setRxCallback(onRxData);

    // Peripheral connection callbacks
    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);

    startAdvertising();

    Serial.println(F("[BLE] NUS peripheral started"));
}

void BleNus::startAdvertising() {
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(uart_);
    Bluefruit.ScanResponse.addName();

    // Auto-restart advertising when disconnected
    Bluefruit.Advertising.restartOnDisconnect(true);
    // Fast: 20ms (32 * 0.625), Slow: 152.5ms (244 * 0.625)
    Bluefruit.Advertising.setInterval(32, 244);
    Bluefruit.Advertising.setFastTimeout(30);  // 30s in fast mode
    Bluefruit.Advertising.start(0);            // Advertise forever
}

// --- Print interface: write into ring buffer ---

size_t BleNus::write(uint8_t c) {
    if (!connected_) return 1;  // Discard silently when not connected
    size_t nextHead = (txHead_ + 1) % TX_BUF_SIZE;
    if (nextHead == txTail_) return 1;  // Buffer full, drop byte
    txBuf_[txHead_] = c;
    txHead_ = nextHead;
    return 1;
}

size_t BleNus::write(const uint8_t* data, size_t size) {
    if (!connected_) return size;
    for (size_t i = 0; i < size; i++) {
        size_t nextHead = (txHead_ + 1) % TX_BUF_SIZE;
        if (nextHead == txTail_) break;  // Buffer full
        txBuf_[txHead_] = data[i];
        txHead_ = nextHead;
    }
    return size;
}

// --- Drain TX ring buffer: send one chunk per call ---

void BleNus::drainTxBuffer() {
    if (!connected_ || txHead_ == txTail_) return;

    BLEConnection* conn = Bluefruit.Connection(connHandle_);
    if (!conn) return;

    uint16_t mtu_payload = conn->getMtu() - 3;
    if (mtu_payload < 20) mtu_payload = 20;

    // Send up to MAX_CHUNKS_PER_UPDATE chunks to improve throughput.
    // BLECharacteristic::notify() blocks on getHvnPacket() (100ms timeout)
    // when the TX queue is full, so sending too many risks blocking the
    // main loop.  4 chunks × 20 bytes = 80 bytes per update at min MTU,
    // or 4 × 244 = 976 bytes at max MTU.  At 66 Hz loop, that's ~5 KB/s
    // (min MTU) or ~64 KB/s (max MTU).
    static const int MAX_CHUNKS_PER_UPDATE = 4;

    for (int i = 0; i < MAX_CHUNKS_PER_UPDATE && txHead_ != txTail_; i++) {
        uint8_t chunk[244];  // Max BLE payload
        size_t count = 0;
        size_t tail = txTail_;

        while (count < mtu_payload && tail != txHead_) {
            chunk[count++] = txBuf_[tail];
            tail = (tail + 1) % TX_BUF_SIZE;
        }

        if (count == 0) break;

        // If notify fails (TX queue full), stop — retry next update()
        if (uart_.write(chunk, count) != count) break;

        txTail_ = tail;
        for (size_t j = 0; j < count; j++) {
            if (chunk[j] == '\n') linesTx_++;
        }
    }
}

void BleNus::update() {
    // Drain TX buffer — send one chunk per loop iteration.
    // This paces output to avoid overwhelming the SoftDevice TX queue.
    drainTxBuffer();
}

void BleNus::writeLine(const char* line) {
    // Write through the Print interface (into ring buffer)
    size_t len = strlen(line);
    if (len > 0) {
        write((const uint8_t*)line, len);
    }
    write((uint8_t)'\n');
}

void BleNus::onConnect(uint16_t conn_hdl) {
    if (!instance_) return;
    instance_->connHandle_ = conn_hdl;
    instance_->connected_ = true;
    instance_->connectTimeMs_ = millis();
    instance_->lineLen_ = 0;
    // Clear TX buffer on new connection
    instance_->txHead_ = 0;
    instance_->txTail_ = 0;

    // Request maximum MTU for better throughput (default is 23, max is 247)
    BLEConnection* conn = Bluefruit.Connection(conn_hdl);
    if (conn) {
        conn->requestMtuExchange(247);
    }

    Serial.println(F("[BLE] NUS client connected"));
}

void BleNus::onDisconnect(uint16_t conn_hdl, uint8_t reason) {
    (void)conn_hdl;
    if (!instance_) return;
    instance_->connHandle_ = BLE_CONN_HANDLE_INVALID;
    instance_->connected_ = false;
    instance_->lineLen_ = 0;
    // Clear TX buffer
    instance_->txHead_ = 0;
    instance_->txTail_ = 0;

    Serial.print(F("[BLE] NUS client disconnected, reason=0x"));
    Serial.println(reason, HEX);
}

void BleNus::onRxData(uint16_t conn_hdl) {
    (void)conn_hdl;
    if (!instance_) return;

    while (instance_->uart_.available()) {
        char c = (char)instance_->uart_.read();
        instance_->processRxByte(c);
    }
}

void BleNus::processRxByte(char c) {
    if (c == '\n' || c == '\r') {
        if (lineLen_ > 0) {
            lineBuf_[lineLen_] = '\0';
            if (lineCallback_) {
                lineCallback_(lineBuf_);
            }
            linesRx_++;
            lineLen_ = 0;
        }
    } else if (lineLen_ < LINE_BUF_SIZE - 1) {
        lineBuf_[lineLen_++] = c;
    }
}

void BleNus::printDiagnostics(Print& out) const {
    // Connection state
    out.print(F("[BLE] NUS: "));
    if (connected_) {
        out.println(F("connected"));
        BLEConnection* conn = Bluefruit.Connection(connHandle_);
        if (conn) {
            out.print(F("[BLE]   MTU="));
            out.print(conn->getMtu());
            out.print(F(" RSSI="));
            out.print(conn->getRssi());
            out.println(F("dBm"));
        }
        out.print(F("[BLE]   conn_uptime="));
        out.print((millis() - connectTimeMs_) / 1000);
        out.println(F("s"));
    } else {
        out.println(F("disconnected"));
    }

    // Advertising state
    out.print(F("[BLE] advertising="));
    out.println(Bluefruit.Advertising.isRunning() ? F("yes") : F("no"));

    // BLE address (for direct connection from ble_dfu.py)
    out.print(F("[BLE] addr="));
    uint8_t mac[6];
    Bluefruit.getAddr(mac);
    for (int i = 5; i >= 0; i--) {
        if (mac[i] < 0x10) out.print('0');
        out.print(mac[i], HEX);
        if (i > 0) out.print(':');
    }
    out.println();

    // TX power
    out.print(F("[BLE] tx_power="));
    out.print(Bluefruit.getTxPower());
    out.println(F("dBm"));

    // Peer count
    out.print(F("[BLE] connections="));
    out.println(Bluefruit.connected());

    // DFU service status
    out.println(F("[BLE] services: NUS DFU DIS"));

    // TX buffer utilization
    size_t used = (txHead_ >= txTail_) ? (txHead_ - txTail_) : (TX_BUF_SIZE - txTail_ + txHead_);
    out.print(F("[BLE] tx_buf="));
    out.print(used);
    out.print(F("/"));
    out.println(TX_BUF_SIZE);

    out.print(F("[BLE] lines_rx="));
    out.print(linesRx_);
    out.print(F(" lines_tx="));
    out.println(linesTx_);
}
