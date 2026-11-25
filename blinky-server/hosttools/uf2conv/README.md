# uf2conv (vendored)

`uf2conv.py` and `LICENSE.txt` are Microsoft's UF2 converter (MIT), copied unmodified from the
Seeeduino nRF52 Arduino core 1.1.10 (`tools/uf2conv/`), the core blinky-firmware's pinned
toolchain installs. sha256 of `uf2conv.py`: `8db5193d06eaca46e1f54a56b0162c50c3e7bb280b2e086c01f670fcc02587e2`.

`../uf2_upload.py` converts a release's hex to UF2 with it (family `0xADA52840`), so flashing
needs no firmware checkout or Arduino toolchain. Checked on 2026-11-24: converting b217's hex
gives a UF2 byte-identical to the release's `blinky-things-b217.uf2`.

The file's `SyntaxWarning: invalid escape sequence '\s'` on Python 3.12+ is upstream's; it is kept
as-is so the converter stays byte-identical to the toolchain copy.
