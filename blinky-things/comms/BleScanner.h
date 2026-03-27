#pragma once
#include <cstddef>
#include <cstdint>

/**
 * BLE Scanner — receives fleet broadcast packets.
 * Stub implementation for CI. Full implementation pending.
 */
class BleScanner {
public:
    using CommandCallback = void (*)(const char* payload, size_t len);

    void begin() {}
    void update() {}
    void setCommandCallback(CommandCallback cb) { (void)cb; }
    void printDiagnostics() const {}
    uint32_t getPacketsReceived() const { return 0; }
    int8_t getLastRssi() const { return 0; }
};
