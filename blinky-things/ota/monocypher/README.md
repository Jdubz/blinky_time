# Monocypher 4.0.2 (vendored)

Ed25519 signature verification (RFC 8032, SHA-512) for signed QSPI OTA
(`ota/OtaManifest.h`). Unmodified copies of:

| File | From the release tarball |
|---|---|
| `monocypher.c`, `monocypher.h` | `src/` |
| `monocypher-ed25519.c`, `monocypher-ed25519.h` | `src/optional/` |
| `LICENCE.md` | top level (BSD-2-Clause or CC0, at our choice) |

Source: https://monocypher.org/download/monocypher-4.0.2.tar.gz, byte-identical
to the GitHub release asset
(https://github.com/LoupVaillant/Monocypher/releases/download/4.0.2/monocypher-4.0.2.tar.gz).
Tarball SHA-512 (matches monocypher.org's published `.sha512`):
`bf275d4c53ff94af6cdc723a4e002e9f080f4d1436c86c76bb37870b34807f1d7b32331d8ff8a1aeb369e946f3769021e03e63efac25b82efc5abf54dc084714`

Compiled as two separate C translation units, `OtaMonocypher.c` and
`OtaMonocypherEd25519.c` at the sketch root (the Arduino build compiles
root-level sources on their own). They must not go through
`BlinkyImplementations.h`, which puts everything in one unit: Monocypher's
internal macros (`FOR`, `COPY`, `MIN`, ...) would leak into the firmware. Excluded
from cppcheck (`.cppcheck-suppressions`): third-party code is not edited to
satisfy our lint. To upgrade, replace the files from a new release tarball,
check its published hash, and update this README.
