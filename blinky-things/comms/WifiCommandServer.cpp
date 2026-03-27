#include "WifiCommandServer.h"
#include "../inputs/SerialConsole.h"

WifiCommandServer::WifiCommandServer(uint16_t port)
    : port_(port), server_(port) {}

void WifiCommandServer::begin() {
    server_.begin();
    server_.setNoDelay(true);
    serverStarted_ = true;
    Serial.print(F("[TCP] Listening on port "));
    Serial.println(port_);
}

bool WifiCommandServer::poll() {
    if (!serverStarted_) return false;

    // Accept new client (non-blocking, replaces existing)
    WiFiClient newClient = server_.available();
    if (newClient) {
        if (client_) client_.stop();
        client_ = newClient;
        client_.setNoDelay(true);
        clientConnected_ = true;
        connectCount_++;
        rxPos_ = 0;
        Serial.print(F("[TCP] Client from "));
        Serial.println(client_.remoteIP());
    }

    // Read data from client (non-blocking)
    bool processed = false;
    if (client_ && client_.connected()) {
        while (client_.available()) {
            uint8_t b = client_.read();
            if (b == '\n' || b == '\r') {
                if (rxPos_ > 0) {
                    rxBuf_[rxPos_] = '\0';
                    linesRx_++;
                    if (console_) {
                        console_->handleCommand(rxBuf_, *this);
                    }
                    rxPos_ = 0;
                    processed = true;
                }
            } else if (rxPos_ < CMD_MAX_LEN - 1) {
                rxBuf_[rxPos_++] = b;
            }
        }
    } else {
        if (clientConnected_) {
            clientConnected_ = false;
            Serial.println(F("[TCP] Client disconnected"));
        }
    }

    return processed;
}

size_t WifiCommandServer::write(uint8_t c) {
    if (client_ && client_.connected()) {
        return client_.write(c);
    }
    return 0;
}

size_t WifiCommandServer::write(const uint8_t* buf, size_t size) {
    if (client_ && client_.connected()) {
        return client_.write(buf, size);
    }
    return 0;
}

void WifiCommandServer::printDiagnostics(Print& out) {
    out.print(F("[TCP] port="));
    out.print(port_);
    out.print(F(" wifi="));
    out.print(isWifiConnected() ? F("yes") : F("no"));
    if (isWifiConnected()) {
        out.print(F(" ip="));
        out.print(WiFi.localIP());
    }
    out.print(F(" client="));
    out.print(clientConnected_ ? F("yes") : F("no"));
    out.print(F(" connects="));
    out.print(connectCount_);
    out.print(F(" rx="));
    out.println(linesRx_);
}

void WifiCommandServer::printDiagnostics() {
    printDiagnostics(Serial);
}
