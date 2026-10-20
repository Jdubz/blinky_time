// Host test for the firmware's signed-OTA manifest check (blinky-things/ota/
// OtaManifest.h), against vectors signed by tools/ota_manifest.py. Proves
// that the device and the release tool agree byte for byte: layout, Ed25519
// (Monocypher vs Python cryptography), SHA-512 streaming, and every policy
// rejection.
//
// Build + run (CI: pr-validation.yml "OTA Manifest"; see tests/ota/README.md).
// Monocypher is compiled from the same root-level C units the firmware uses.

#include <stdio.h>
#include <string.h>

#include "ota/OtaManifest.h"
#include "vectors.h"

static int failures = 0;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            printf("FAIL %s:%d ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
            failures++;                          \
        }                                        \
    } while (0)

int main() {
    const uint32_t maxSize = 0xCD000;

    for (const VecCase& c : VEC_CASES) {
        ota::Manifest m;
        ota::Verdict v = ota::parseAndVerify(c.manifest, ota::MANIFEST_LEN, VEC_PUBKEY,
                                             ota::BOARD_XIAO_NRF52840_SENSE, VEC_CURRENT_BUILD,
                                             maxSize, m);
        char expected[32];
        // vectors.h names verdicts like the enum (OK, BAD_SIGNATURE, ...);
        // verdictName() is the wire spelling (ok, bad-signature, ...).
        snprintf(expected, sizeof(expected), "%s", c.verdict);
        for (char* p = expected; *p; ++p) *p = (*p == '_') ? '-' : (char)(*p | 0x20);
        CHECK(strcmp(ota::verdictName(v), expected) == 0, "%s: got %s, expected %s", c.name,
              ota::verdictName(v), expected);
        printf("%-24s %s\n", c.name, ota::verdictName(v));
    }

    // A valid manifest decodes to the fields the tool signed.
    ota::Manifest m;
    CHECK(ota::parseAndVerify(VEC_VALID_UPGRADE, ota::MANIFEST_LEN, VEC_PUBKEY,
                              ota::BOARD_XIAO_NRF52840_SENSE, VEC_CURRENT_BUILD, maxSize,
                              m) == ota::Verdict::OK,
          "valid manifest rejected");
    CHECK(m.build == 215 && m.size == sizeof(VEC_IMAGE) && m.loadAddr == ota::APP_LOAD_ADDR,
          "decoded fields: build %u size %u load 0x%x", (unsigned)m.build, (unsigned)m.size,
          (unsigned)m.loadAddr);

    // The firmware hashes the image as it streams in, in uneven chunks.
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    size_t off = 0, step = 1;
    while (off < sizeof(VEC_IMAGE)) {
        size_t n = sizeof(VEC_IMAGE) - off < step ? sizeof(VEC_IMAGE) - off : step;
        crypto_sha512_update(&ctx, VEC_IMAGE + off, n);
        off += n;
        step = step * 3 + 1;
    }
    uint8_t digest[64];
    crypto_sha512_final(&ctx, digest);
    CHECK(memcmp(digest, m.sha512, 64) == 0, "streamed SHA-512 differs from the manifest");

    // Every length but MANIFEST_LEN is refused before anything is parsed.
    CHECK(ota::parseAndVerify(VEC_VALID_UPGRADE, ota::MANIFEST_LEN - 1, VEC_PUBKEY,
                              ota::BOARD_XIAO_NRF52840_SENSE, VEC_CURRENT_BUILD, maxSize,
                              m) == ota::Verdict::BAD_LENGTH,
          "short manifest accepted");
    // Size ceiling.
    CHECK(ota::parseAndVerify(VEC_VALID_UPGRADE, ota::MANIFEST_LEN, VEC_PUBKEY,
                              ota::BOARD_XIAO_NRF52840_SENSE, VEC_CURRENT_BUILD, 100,
                              m) == ota::Verdict::BAD_SIZE,
          "oversize image accepted");

    // Hex decoding used by `ota manifest <hex>`.
    uint8_t out[2];
    CHECK(ota::hexDecode("0aFf", 4, out, 2) && out[0] == 0x0a && out[1] == 0xff, "hexDecode");
    CHECK(!ota::hexDecode("0g00", 4, out, 2), "hexDecode accepted a non-hex char");
    CHECK(!ota::hexDecode("0a0", 3, out, 2), "hexDecode accepted a short string");

    if (failures) {
        printf("%d FAILURE(S)\n", failures);
        return 1;
    }
    printf("all OTA manifest checks passed\n");
    return 0;
}
