#include "WifiCommandServer.h"
#include "../inputs/SerialConsole.h"

WifiCommandServer::WifiCommandServer(uint16_t port)
    : server_(port), port_(port) {
    memset(rxBuf_, 0, sizeof(rxBuf_));
}

void WifiCommandServer::begin() {
    if (WiFi.status() == WL_CONNECTED) {
        server_.begin();
        started_ = true;
        Serial.print(F("[TCP] Server listening on port "));
        Serial.println(port_);
    } else {
        Serial.println(F("[TCP] WiFi not connected — server deferred"));
    }
}

void WifiCommandServer::update() {
    // Start server if WiFi came up after begin()
    if (!started_ && WiFi.status() == WL_CONNECTED) {
        server_.begin();
        started_ = true;
        Serial.print(F("[TCP] Server started on "));
        Serial.print(WiFi.localIP());
        Serial.print(F(":"));
        Serial.println(port_);
    }

    if (!started_) return;

    // Accept new client (one at a time)
    if (!client_ || !client_.connected()) {
        WiFiClient newClient = server_.available();
        if (newClient) {
            // Disconnect old client if any
            if (client_) client_.stop();
            client_ = newClient;
            client_.setNoDelay(true);  // Disable Nagle for responsive commands
            connectCount_++;
            rxPos_ = 0;
            Serial.print(F("[TCP] Client connected from "));
            Serial.println(client_.remoteIP());
        }
    }

    // Read data from connected client
    if (client_ && client_.connected()) {
        while (client_.available()) {
            uint8_t b = client_.read();
            processRxByte(b);
        }
    }
}

void WifiCommandServer::processRxByte(uint8_t b) {
    if (b == '\n' || b == '\r') {
        if (rxPos_ > 0) {
            rxBuf_[rxPos_] = '\0';
            linesRx_++;

            // Route command through SerialConsole.
            // The response goes to the TeeStream (Serial + BLE/TCP secondary).
            // We set ourselves as the secondary to capture the output.
            if (console_) {
                console_->handleCommand(rxBuf_, *this);
            }

            rxPos_ = 0;
        }
    } else if (rxPos_ < RX_BUF_SIZE - 1) {
        rxBuf_[rxPos_++] = b;
    }
}

size_t WifiCommandServer::write(uint8_t c) {
    if (client_ && client_.connected()) {
        if (c == '\n') linesTx_++;
        return client_.write(c);
    }
    return 0;
}

size_t WifiCommandServer::write(const uint8_t* buf, size_t size) {
    if (client_ && client_.connected()) {
        for (size_t i = 0; i < size; i++) {
            if (buf[i] == '\n') linesTx_++;
        }
        return client_.write(buf, size);
    }
    return 0;
}

void WifiCommandServer::printDiagnostics() {
    Serial.print(F("[TCP] port="));
    Serial.print(port_);
    Serial.print(F(" started="));
    Serial.print(started_ ? F("yes") : F("no"));
    Serial.print(F(" client="));
    Serial.print(hasClient() ? F("connected") : F("none"));
    Serial.print(F(" connects="));
    Serial.print(connectCount_);
    Serial.print(F(" rx="));
    Serial.print(linesRx_);
    Serial.print(F(" tx="));
    Serial.println(linesTx_);
}
