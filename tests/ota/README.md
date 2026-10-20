# Signed OTA manifest test

Checks that the firmware's manifest verifier (`blinky-things/ota/OtaManifest.h`,
with the vendored Monocypher) and the release tool (`tools/ota_manifest.py`,
Python `cryptography`) agree byte for byte. The vectors are signed with a
fixed, public test key; never a release key.

```bash
python tools/ota_manifest.py vectors --out tests/ota/vectors.h   # regenerate (committed)
mkdir -p build
gcc -c -O1 -I blinky-things blinky-things/OtaMonocypher.c        -o build/mc.o
gcc -c -O1 -I blinky-things blinky-things/OtaMonocypherEd25519.c -o build/mc_ed.o
g++ -std=gnu++11 -Wall -Wextra -O1 -I blinky-things -I tests/ota \
    tests/ota/ota_manifest_test.cpp build/mc.o build/mc_ed.o -o build/ota_manifest_test
build/ota_manifest_test
```

On the Windows workstation, use `python -m ziglang cc` / `python -m ziglang c++`
for `gcc` / `g++`.
