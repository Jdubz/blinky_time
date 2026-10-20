#pragma once
/**
 * Public keys the app trusts for signed QSPI OTA (ota/OtaManifest.h).
 *
 * RELEASE key: the private half is the secret BLINKY_RELEASE_SIGNING_KEY of
 * the GitHub environment `release-signing` (master only), plus the
 * maintainer's offline backup. sign-release.yml signs a release with it only
 * after its bench gate passes (tools/release_promote.py); candidates are never
 * signed.
 * Rotating it means shipping, signed with the OLD key, a firmware that
 * carries the new public key here.
 *
 * BENCH key: for testing the update path on a test device with a local
 * `scripts/build.sh --dev --ota-bench-key` build, without ever signing a
 * dev build with the release key. The private half lives only on the
 * bench machine (~/.blinky/ota-bench-key.pem). A build with the bench key
 * must carry a `-dev-` version, so it can never be released.
 */

#include <stdint.h>

#include "../types/Version.h"

namespace ota {

#ifdef BLINKY_OTA_BENCH_KEY
namespace detail {
constexpr bool hasDevTag(const char* s) {
    return *s != '\0' &&
           ((s[0] == '-' && s[1] == 'd' && s[2] == 'e' && s[3] == 'v' && s[4] == '-') ||
            hasDevTag(s + 1));
}
}  // namespace detail
static_assert(detail::hasDevTag(FIRMWARE_VERSION),
              "BLINKY_OTA_BENCH_KEY is only allowed in -dev- builds (scripts/build.sh --dev)");

// e3f83b8228c6fc8b800c31a7ded171c0b5fd92450021c99b91d26ac09134f658
static const uint8_t TRUSTED_PUBKEY[32] = {
    0xe3, 0xf8, 0x3b, 0x82, 0x28, 0xc6, 0xfc, 0x8b, 0x80, 0x0c, 0x31, 0xa7, 0xde, 0xd1, 0x71, 0xc0,
    0xb5, 0xfd, 0x92, 0x45, 0x00, 0x21, 0xc9, 0x9b, 0x91, 0xd2, 0x6a, 0xc0, 0x91, 0x34, 0xf6, 0x58,
};
static const char* const TRUSTED_KEY_NAME = "bench";
#else
// fd0e5a17fdd37cd7baaf7cddef5201679e07183bb4480625164b8d6cbc8301ab
static const uint8_t TRUSTED_PUBKEY[32] = {
    0xfd, 0x0e, 0x5a, 0x17, 0xfd, 0xd3, 0x7c, 0xd7, 0xba, 0xaf, 0x7c, 0xdd, 0xef, 0x52, 0x01, 0x67,
    0x9e, 0x07, 0x18, 0x3b, 0xb4, 0x48, 0x06, 0x25, 0x16, 0x4b, 0x8d, 0x6c, 0xbc, 0x83, 0x01, 0xab,
};
static const char* const TRUSTED_KEY_NAME = "release";
#endif

}  // namespace ota
