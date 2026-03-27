#pragma once

/**
 * BleProtocol.h - Shared BLE advertising packet format
 *
 * Defines the manufacturer-specific data format used for fleet communication:
 * - ESP32-S3 broadcasts settings/commands as BLE extended advertising packets
 * - nRF52840 devices passively scan and apply matching packets
 *
 * Packet layout (inside Manufacturer Specific Data, AD type 0xFF):
 *   Bytes 0-1: Company ID (0xFFFF = reserved for testing/private use)
 *   Byte  2:   Protocol version
 *   Byte  3:   Packet type (settings, scene, command)
 *   Byte  4:   Sequence number (0-255, rolling, for dedup on receiver)
 *   Byte  5:   Fragment info [total:4][index:4] (0x10 = unfragmented)
 *   Bytes 6-N: Payload (JSON or compact binary, up to ~240 bytes)
 */

#include <stdint.h>

namespace BleProtocol {

static const uint16_t COMPANY_ID = 0xFFFF;       // Private/testing use
static const uint8_t PROTOCOL_VERSION = 0x01;

enum PacketType : uint8_t {
    SETTINGS = 0x01,   // JSON settings blob
    SCENE    = 0x02,   // Scene/generator change
    COMMAND  = 0x03,   // Arbitrary serial command string
};

struct __attribute__((packed)) Header {
    uint8_t version;
    uint8_t type;
    uint8_t sequence;
    uint8_t fragment;   // High nibble = total fragments, low nibble = fragment index
};

static const size_t HEADER_SIZE = sizeof(Header);  // 4 bytes

// BLE 5 extended advertising allows up to 255 bytes of manufacturer data.
// Subtract 2 (company ID) + 4 (header) = 249 usable payload bytes.
// Round down for safety.
static const size_t MAX_PAYLOAD = 240;

// Fragment helpers
inline uint8_t makeFragment(uint8_t total, uint8_t index) {
    return (total << 4) | (index & 0x0F);
}
inline uint8_t fragmentTotal(uint8_t f) { return f >> 4; }
inline uint8_t fragmentIndex(uint8_t f) { return f & 0x0F; }

// Unfragmented single packet
static const uint8_t FRAGMENT_SINGLE = makeFragment(1, 0);  // 0x10

}  // namespace BleProtocol
