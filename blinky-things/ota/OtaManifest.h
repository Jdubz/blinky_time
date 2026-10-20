#pragma once
/**
 * OtaManifest — the signed description of a firmware image that the app
 * must receive and verify BEFORE it stages a single byte of that image in
 * QSPI (docs/WEB_FLASHER.md, "Signed OTA").
 *
 * Wire format: 152 bytes, little-endian, sent as hex in `ota manifest <hex>`.
 *   off  size  field
 *     0     4  magic        "BLOT" (0x544F4C42)
 *     4     2  format       MANIFEST_FORMAT
 *     6     2  flags        FLAG_EMERGENCY_ROLLBACK may be set
 *     8     4  board        BOARD_XIAO_NRF52840_SENSE
 *    12     4  build        firmware build number (FIRMWARE_BUILD of the image)
 *    16     4  size         image bytes (raw application binary)
 *    20     4  loadAddr     must be APP_LOAD_ADDR (where the bootloader copies it)
 *    24    64  sha512       SHA-512 of the image
 *    88    64  signature    Ed25519 (RFC 8032) over bytes [0, 88)
 *
 * Produced and signed only by tools/ota_manifest.py in the release workflow
 * (key: GitHub secret BLINKY_RELEASE_SIGNING_KEY). Pure C++ with no Arduino
 * dependencies, so tests/ota/ can compile it on the host against vectors
 * signed by that same tool.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "monocypher/monocypher-ed25519.h"

namespace ota {

constexpr uint32_t MANIFEST_MAGIC = 0x544F4C42;  // "BLOT"
constexpr uint16_t MANIFEST_FORMAT = 1;
constexpr size_t MANIFEST_SIGNED_LEN = 88;
constexpr size_t MANIFEST_LEN = 152;
constexpr uint16_t FLAG_EMERGENCY_ROLLBACK = 0x0001;
constexpr uint16_t KNOWN_FLAGS = FLAG_EMERGENCY_ROLLBACK;
constexpr uint32_t BOARD_XIAO_NRF52840_SENSE = 1;
// End of S140 7.3.0 = start of the application (tools/uf2_upload.py SD_END_S140_730).
constexpr uint32_t APP_LOAD_ADDR = 0x27000;

struct Manifest {
    uint16_t format = 0;
    uint16_t flags = 0;
    uint32_t board = 0;
    uint32_t build = 0;
    uint32_t size = 0;
    uint32_t loadAddr = 0;
    uint8_t sha512[64] = {};
};

enum class Verdict : uint8_t {
    OK,
    BAD_LENGTH,
    BAD_MAGIC,
    BAD_FORMAT,
    UNKNOWN_FLAGS,
    BAD_SIGNATURE,
    WRONG_BOARD,
    BAD_LOAD_ADDR,
    BAD_SIZE,
    NOT_NEWER,
};

inline const char* verdictName(Verdict v) {
    switch (v) {
        case Verdict::OK:            return "ok";
        case Verdict::BAD_LENGTH:    return "bad-length";
        case Verdict::BAD_MAGIC:     return "bad-magic";
        case Verdict::BAD_FORMAT:    return "bad-format";
        case Verdict::UNKNOWN_FLAGS: return "unknown-flags";
        case Verdict::BAD_SIGNATURE: return "bad-signature";
        case Verdict::WRONG_BOARD:   return "wrong-board";
        case Verdict::BAD_LOAD_ADDR: return "bad-load-addr";
        case Verdict::BAD_SIZE:      return "bad-size";
        case Verdict::NOT_NEWER:     return "not-newer";
    }
    return "unhandled-verdict";  // unreachable: every enumerator is listed above
}

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Verify a raw manifest and decode it into `out`. The signature is checked
 * before any field is trusted; after it, every policy check must pass:
 * our board, the application load address, 0 < size <= maxSize, and a build
 * strictly newer than `currentBuild` unless the (signed) emergency-rollback
 * flag is set. `out` is only meaningful when the result is Verdict::OK.
 */
inline Verdict parseAndVerify(const uint8_t* raw, size_t len, const uint8_t pubkey[32],
                              uint32_t board, uint32_t currentBuild, uint32_t maxSize,
                              Manifest& out) {
    if (len != MANIFEST_LEN) return Verdict::BAD_LENGTH;
    if (rd32(raw) != MANIFEST_MAGIC) return Verdict::BAD_MAGIC;
    if (rd16(raw + 4) != MANIFEST_FORMAT) return Verdict::BAD_FORMAT;
    if (crypto_ed25519_check(raw + MANIFEST_SIGNED_LEN, pubkey, raw, MANIFEST_SIGNED_LEN) != 0) {
        return Verdict::BAD_SIGNATURE;
    }
    Manifest m;
    m.format = rd16(raw + 4);
    m.flags = rd16(raw + 6);
    m.board = rd32(raw + 8);
    m.build = rd32(raw + 12);
    m.size = rd32(raw + 16);
    m.loadAddr = rd32(raw + 20);
    memcpy(m.sha512, raw + 24, sizeof(m.sha512));
    // Signed but unknown flags mean a newer manifest format than this
    // firmware understands: refuse rather than ignore them.
    if (m.flags & ~KNOWN_FLAGS) return Verdict::UNKNOWN_FLAGS;
    if (m.board != board) return Verdict::WRONG_BOARD;
    if (m.loadAddr != APP_LOAD_ADDR) return Verdict::BAD_LOAD_ADDR;
    if (m.size == 0 || m.size > maxSize) return Verdict::BAD_SIZE;
    if (m.build <= currentBuild && !(m.flags & FLAG_EMERGENCY_ROLLBACK)) return Verdict::NOT_NEWER;
    out = m;
    return Verdict::OK;
}

/** Decode exactly `outLen` bytes of hex. False on any non-hex char or wrong length. */
inline bool hexDecode(const char* hex, size_t hexLen, uint8_t* out, size_t outLen) {
    if (hexLen != outLen * 2) return false;
    for (size_t i = 0; i < outLen; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[i * 2 + k];
            uint8_t d;
            if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
            else return false;
            v = (uint8_t)((v << 4) | d);
        }
        out[i] = v;
    }
    return true;
}

}  // namespace ota
