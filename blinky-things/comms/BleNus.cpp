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

    // Advertising is started separately (startAdvertising) once the sketch
    // has registered all services.
    Serial.println(F("[BLE] NUS peripheral started"));
}

void BleNus::startAdvertising() {
    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addTxPower();
    Bluefruit.Advertising.addService(uart_);
    Bluefruit.ScanResponse.addName();

    // Initial gossip-ACK manufacturer data block in the scan response
    // (BLE_FLEET_RELIABILITY_PLAN.md item #5). Encodes the last accepted
    // COMMAND_V2 command_id so a scanning server can detect lagged
    // devices and re-broadcast the missed command. Format (5 bytes):
    //   [company_id LE = 0xFFFF (2B)] [marker = 0xA0 ACK (1B)]
    //   [command_id LE (2B)]
    //
    // Placed in scan response (not primary adv) because the primary
    // adv is near-full with flags + tx-power + NUS UUID; the scan
    // response has the 24-char device name (~26 bytes) plus comfortable
    // headroom. Server-side scanner must use active scanning to read
    // it (already the case for fleet discovery).
    //
    // Initial command_id = 0 (no command yet applied; matches the
    // broadcaster's starting state). setLastAckedCommandId() updates
    // this block when a new COMMAND_V2 is dispatched.
    uint8_t ackData[5] = {0xFF, 0xFF, 0xA0, 0x00, 0x00};
    Bluefruit.ScanResponse.addManufacturerData(ackData, sizeof(ackData));

    // Auto-restart advertising when disconnected
    Bluefruit.Advertising.restartOnDisconnect(true);
    // Fast: 20ms (32 * 0.625), Slow: 152.5ms (244 * 0.625)
    Bluefruit.Advertising.setInterval(32, 244);
    Bluefruit.Advertising.setFastTimeout(30);  // 30s in fast mode
    Bluefruit.Advertising.start(0);            // Advertise forever
}

void BleNus::setLastAckedCommandId(uint16_t newId) {
    if (newId == lastAdvertisedCmdId_) {
        return;  // No change — skip the expensive scan-response rebuild.
    }
    lastAdvertisedCmdId_ = newId;

    // Rebuild the scan response payload. Bluefruit's ScanResponse data
    // is mutable: clearData() drops the cached AD blob, addName() +
    // addManufacturerData() rebuild it, and the next advertising
    // emission picks up the new bytes. We do NOT stop/restart the
    // primary adv — only the scan response is rebuilt, so connected
    // NUS clients are unaffected.
    //
    // IMPORTANT: the rebuild list below MUST stay in sync with
    // startAdvertising()'s ScanResponse contents. clearData() drops
    // EVERY AD structure in the scan response; anything added in
    // startAdvertising() that isn't re-added here will be silently
    // missing from subsequent ad cycles. Today the scan response has
    // just two entries (Name + ACK manufacturer data); if more are
    // added in startAdvertising(), mirror them here.
    uint8_t ackData[5] = {0xFF, 0xFF, 0xA0,
                          static_cast<uint8_t>(newId & 0xFF),
                          static_cast<uint8_t>((newId >> 8) & 0xFF)};
    Bluefruit.ScanResponse.clearData();
    Bluefruit.ScanResponse.addName();
    Bluefruit.ScanResponse.addManufacturerData(ackData, sizeof(ackData));
}

// --- Print interface: write into ring buffer ---

size_t BleNus::write(uint8_t c) {
    return write(&c, 1);
}

size_t BleNus::write(const uint8_t* data, size_t size) {
    // Not connected: no central to deliver to — discarding is correct, not a loss.
    if (!connected_) return size;
    for (size_t i = 0; i < size; i++) {
        size_t nextHead = (txHead_ + 1) % TX_BUF_SIZE;
        if (nextHead == txTail_) {
            // Ring full: the central isn't draining fast enough. The rest of
            // this write is lost (mid-line), so count it and say so once —
            // a truncated JSON line downstream must be attributable.
            txBytesDropped_ += (uint32_t)(size - i);
            if (!txDropLogged_) {
                txDropLogged_ = true;
                Serial.println(F("[FALLBACK] BleNus TX ring full — dropping output (see 'ble' tx_dropped)"));
            }
            break;
        }
        txBuf_[txHead_] = data[i];
        txHead_ = nextHead;
    }
    // Report full size: Print callers don't handle short writes, and the
    // loss is accounted for above.
    return size;
}

uint16_t BleNus::currentMtu() const {
    if (!connected_) return 0;
    BLEConnection* conn = Bluefruit.Connection(connHandle_);
    if (!conn) return 0;
    const uint16_t mtu = conn->getMtu();
    return mtu < MAX_ATT_MTU ? mtu : MAX_ATT_MTU;
}

// --- Non-blocking notify (#189) ---
//
// BLEConnection keeps its HVN-buffer semaphore private and offers only getHvnPacket(),
// which waits up to BLE_GENERIC_TIMEOUT (100 ms). Called from the loop task while the
// record sender (another task) fills the same HVN queue, that wait stalled the loop. The
// explicit-instantiation rule (access is not checked on the template arguments of an
// explicit instantiation, C++11 [temp.explicit]/12) yields a pointer to that member, so the
// loop can take a buffer without waiting and send exactly as notify() does. If Bluefruit
// renames the member this fails to compile rather than misbehave.
namespace {
struct HvnSem {
    typedef SemaphoreHandle_t BLEConnection::*type;
    friend type hvnSemMember(HvnSem);
};
template <typename Tag, typename Tag::type Member>
struct ExposeMember {
    friend typename Tag::type hvnSemMember(Tag) { return Member; }
};
template struct ExposeMember<HvnSem, &BLEConnection::_hvn_sem>;
}  // namespace

bool BleNus::notifyNoWait(BLEConnection* conn, const uint8_t* data, uint16_t len) {
    if (!uart_.notifyEnabled(connHandle_)) return false;
    SemaphoreHandle_t sem = conn->*hvnSemMember(HvnSem());
    if (xSemaphoreTake(sem, 0) != pdTRUE) return false;  // queue full: next update() retries
    uint16_t sent = len;
    ble_gatts_hvx_params_t hvx = {};
    hvx.handle = uart_.txValueHandle();
    hvx.type = BLE_GATT_HVX_NOTIFICATION;
    hvx.offset = 0;
    hvx.p_len = &sent;
    hvx.p_data = data;
    if (sd_ble_gatts_hvx(connHandle_, &hvx) != NRF_SUCCESS) {
        xSemaphoreGive(sem);  // nothing queued: the buffer's completion will never come
        return false;
    }
    // Queued: Bluefruit gives the buffer back on BLE_GATTS_EVT_HVN_TX_COMPLETE, as for
    // notify(). A notification is sent whole or not at all at len <= MTU - 3.
    return sent == len;
}

// --- Drain TX ring buffer: send one chunk per call ---

void BleNus::drainTxBuffer() {
    if (!connected_ || txHead_ == txTail_) return;

    BLEConnection* conn = Bluefruit.Connection(connHandle_);
    if (!conn) return;

    // Capped at the SoftDevice's ceiling (#173): getMtu() reported 527 for
    // a Windows central, and copying 524 bytes into the 244-byte chunk below
    // smashed the stack and reset the device on any reply longer than ~244 B.
    uint16_t mtu_payload = static_cast<uint16_t>(currentMtu() - 3);
    if (mtu_payload < 20) mtu_payload = 20;

    // Send up to MAX_CHUNKS_PER_UPDATE chunks to improve throughput. Each is
    // sent without waiting (notifyNoWait): a full HVN queue ends this update
    // and the rest waits in the ring, so the loop never stalls on BLE (#189).
    // 4 chunks × 20 bytes = 80 bytes per update at min MTU, or 4 × 244 = 976
    // bytes at max MTU. At 66 Hz loop, that's ~5 KB/s (min MTU) or ~64 KB/s
    // (max MTU).
    static const int MAX_CHUNKS_PER_UPDATE = 4;

    for (int i = 0; i < MAX_CHUNKS_PER_UPDATE && txHead_ != txTail_; i++) {
        uint8_t chunk[MAX_ATT_MTU - 3];  // max notification payload
        static_assert(sizeof(chunk) == MAX_ATT_MTU - 3, "chunk sized to the SoftDevice MTU");
        size_t count = 0;
        size_t tail = txTail_;

        while (count < mtu_payload && count < sizeof(chunk) && tail != txHead_) {
            chunk[count++] = txBuf_[tail];
            tail = (tail + 1) % TX_BUF_SIZE;
        }

        if (count == 0) break;

        // No free HVN buffer (or the central unsubscribed): stop, retry next update()
        if (!notifyNoWait(conn, chunk, static_cast<uint16_t>(count))) break;

        txTail_ = tail;
        for (size_t j = 0; j < count; j++) {
            if (chunk[j] == '\n') linesTx_++;
        }
    }
}

void BleNus::update() {
    // Commands received over NUS run here, on the loop task (#173).
    runQueuedCommands();
    // Drain TX buffer — send one chunk per loop iteration.
    // This paces output to avoid overwhelming the SoftDevice TX queue.
    drainTxBuffer();
}

void BleNus::runQueuedCommands() {
    // Bounded: lines queued while these run wait for the next loop tick.
    for (uint8_t n = 0; n < CMD_QUEUE_DEPTH; n++) {
        const uint8_t tail = cmdTail_;
        if (tail == cmdHead_) break;  // empty
        __DMB();                      // read the slot only after seeing it published
        if (lineCallback_) lineCallback_(cmdQueue_[tail]);
        __DMB();                      // finish with the slot before freeing it
        cmdTail_ = static_cast<uint8_t>((tail + 1) % CMD_QUEUE_DEPTH);
    }
    const uint32_t dropped = cmdDropped_;
    if (dropped != cmdDroppedReported_) {
        Serial.print(F("[BLE] NUS command queue full: "));
        Serial.print(dropped - cmdDroppedReported_);
        Serial.println(F(" line(s) dropped"));
        cmdDroppedReported_ = dropped;
    }
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

    // Request maximum MTU for better throughput (default is 23, max is 247).
    // Only effective because setup() calls configPrphBandwidth(BANDWIDTH_MAX)
    // before Bluefruit.begin() — otherwise the SoftDevice ceiling is 23.
    // (2M PHY + Data Length Extension are requested only when a record
    // session starts — RecordStream::start — so the fleet server's link
    // keeps its existing PHY/data-length behaviour.)
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
            // Queue for the loop task; never run the command here (#173).
            const uint8_t head = cmdHead_;
            const uint8_t next = static_cast<uint8_t>((head + 1) % CMD_QUEUE_DEPTH);
            if (next == cmdTail_) {
                cmdDropped_ = cmdDropped_ + 1;  // full: keep order, drop the newest
            } else {
                memcpy(cmdQueue_[head], lineBuf_, lineLen_ + 1);
                __DMB();  // slot contents visible before it's published
                cmdHead_ = next;
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
            out.print(F(" PHY="));
            out.print(conn->getPHY());
            out.print(F(" DLE="));
            out.print(conn->getDataLength());
            out.print(F(" interval="));
            out.print(conn->getConnectionInterval() * 1.25f, 2);
            out.print(F("ms RSSI="));
            out.print(conn->getRssi());
            out.println(F("dBm"));
        }
        out.print(F("[BLE]   tx_dropped="));
        out.println(txBytesDropped_);
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
    out.print(linesTx_);
    out.print(F(" cmd_dropped="));
    out.println(cmdDropped_);
}
