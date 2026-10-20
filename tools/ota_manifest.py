#!/usr/bin/env python3
"""
ota_manifest.py - build, sign and verify signed QSPI OTA manifests.

The firmware (blinky-things/ota/OtaManifest.h) refuses to stage an image in
QSPI unless it first receives a 152-byte manifest signed with the release
key: magic "BLOT", format, flags, board, build, size, load address, SHA-512
of the image, Ed25519 signature over the first 88 bytes. Layout is
documented in OtaManifest.h; this file is the only producer.

The image is the raw application binary the bootloader copies to 0x27000:
the release hex from its first byte (which must be 0x27000) to its last,
gaps filled with 0xFF (erased flash).

CLI:
  ota_manifest.py sign --hex H --build N --out-dir D [--key-env VAR | --key-file F]
      writes blinky-things-b<N>.bin and blinky-things-b<N>.otam
  ota_manifest.py verify --manifest M --bin B --pubkey HEX [--current-build N]
  ota_manifest.py vectors --out tests/ota/vectors.h
      deterministic test vectors (fixed test key) for tests/ota/ota_manifest_test.cpp

Every failure exits non-zero with a message; nothing falls back silently.
"""

import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

MAGIC = 0x544F4C42  # "BLOT"
FORMAT = 1
SIGNED_LEN = 88
MANIFEST_LEN = 152
FLAG_EMERGENCY_ROLLBACK = 0x0001
BOARD_XIAO_NRF52840_SENSE = 1
APP_LOAD_ADDR = 0x27000
# QspiOtaStaging::MAX_FW_SIZE (and the bootloader's own 0xCD000 ceiling)
MAX_FW_SIZE = 0xCD000


class OtaError(Exception):
    pass


# ------------------------------------------------------------------ image ---

def hex_to_image(text):
    """Strict Intel HEX -> (load address, bytes). Gaps are 0xFF (erased flash)."""
    chunks, base, eof = [], 0, False
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if eof:
            raise OtaError(f"hex line {n}: data after EOF")
        if not line.startswith(":") or len(line) < 11 or len(line) % 2 == 0:
            raise OtaError(f"hex line {n}: not an Intel HEX record")
        try:
            rec = bytes.fromhex(line[1:])
        except ValueError:
            raise OtaError(f"hex line {n}: non-hex character")
        if len(rec) != rec[0] + 5 or sum(rec) & 0xFF:
            raise OtaError(f"hex line {n}: bad length or checksum")
        count, addr, rtype, data = rec[0], (rec[1] << 8) | rec[2], rec[3], rec[4:4 + rec[0]]
        if rtype == 0x00:
            chunks.append((base + addr, data))
        elif rtype == 0x01:
            eof = True
        elif rtype == 0x02 and count == 2:
            base = ((data[0] << 8) | data[1]) * 16
        elif rtype == 0x04 and count == 2:
            base = ((data[0] << 8) | data[1]) << 16
        elif rtype in (0x03, 0x05):
            pass
        else:
            raise OtaError(f"hex line {n}: unsupported record type {rtype}")
    if not eof:
        raise OtaError("hex has no EOF record")
    if not chunks:
        raise OtaError("hex has no data")
    chunks.sort()
    start = chunks[0][0]
    end = max(a + len(d) for a, d in chunks)
    image = bytearray(b"\xff" * (end - start))
    filled = bytearray(end - start)
    for a, d in chunks:
        if any(filled[a - start:a - start + len(d)]):
            raise OtaError(f"hex data overlaps at 0x{a:08X}")
        image[a - start:a - start + len(d)] = d
        filled[a - start:a - start + len(d)] = b"\x01" * len(d)
    return start, bytes(image)


# --------------------------------------------------------------- manifest ---

def unsigned_manifest(image, build, board=BOARD_XIAO_NRF52840_SENSE, flags=0, load_addr=APP_LOAD_ADDR):
    if not image or len(image) > MAX_FW_SIZE:
        raise OtaError(f"image size {len(image)} outside 1..{MAX_FW_SIZE}")
    if not 0 < build < 2**32:
        raise OtaError(f"build {build} out of range")
    body = struct.pack("<IHHIIII", MAGIC, FORMAT, flags, board, build, len(image), load_addr)
    body += hashlib.sha512(image).digest()
    assert len(body) == SIGNED_LEN
    return body


def sign(body, key):
    if len(body) != SIGNED_LEN:
        raise OtaError(f"manifest body is {len(body)} bytes, expected {SIGNED_LEN}")
    m = body + key.sign(body)
    assert len(m) == MANIFEST_LEN
    return m


def decode(manifest):
    if len(manifest) != MANIFEST_LEN:
        raise OtaError(f"manifest is {len(manifest)} bytes, expected {MANIFEST_LEN}")
    magic, fmt, flags, board, build, size, load = struct.unpack_from("<IHHIIII", manifest)
    return {"magic": magic, "format": fmt, "flags": flags, "board": board, "build": build,
            "size": size, "load_addr": load, "sha512": manifest[24:88]}


def verify(manifest, pubkey_bytes, image=None, current_build=None):
    """Same checks, in the same order, as ota::parseAndVerify (+ the image hash)."""
    f = decode(manifest)
    if f["magic"] != MAGIC:
        raise OtaError("bad magic")
    if f["format"] != FORMAT:
        raise OtaError(f"unknown format {f['format']}")
    try:
        Ed25519PublicKey.from_public_bytes(pubkey_bytes).verify(manifest[SIGNED_LEN:], manifest[:SIGNED_LEN])
    except Exception:
        raise OtaError("bad signature")
    if f["flags"] & ~FLAG_EMERGENCY_ROLLBACK:
        raise OtaError(f"unknown flags 0x{f['flags']:04X}")
    if f["board"] != BOARD_XIAO_NRF52840_SENSE:
        raise OtaError(f"wrong board {f['board']}")
    if f["load_addr"] != APP_LOAD_ADDR:
        raise OtaError(f"bad load address 0x{f['load_addr']:X}")
    if not 0 < f["size"] <= MAX_FW_SIZE:
        raise OtaError(f"bad size {f['size']}")
    if current_build is not None and f["build"] <= current_build and not f["flags"] & FLAG_EMERGENCY_ROLLBACK:
        raise OtaError(f"build {f['build']} is not newer than {current_build}")
    if image is not None:
        if len(image) != f["size"]:
            raise OtaError(f"image is {len(image)} bytes, manifest says {f['size']}")
        if hashlib.sha512(image).digest() != f["sha512"]:
            raise OtaError("image SHA-512 does not match the manifest")
    return f


# -------------------------------------------------------------------- keys ---

def load_key(key_env=None, key_file=None):
    if key_env:
        pem = os.environ.get(key_env)
        if not pem:
            raise OtaError(f"signing key env var {key_env} is empty or unset")
        pem = pem.encode()
    elif key_file:
        pem = Path(key_file).read_bytes()
    else:
        raise OtaError("no signing key given (--key-env or --key-file)")
    key = serialization.load_pem_private_key(pem, password=None)
    if not isinstance(key, Ed25519PrivateKey):
        raise OtaError("signing key is not an Ed25519 private key")
    return key


def public_hex(key):
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw).hex()


# ----------------------------------------------------------------- vectors ---

TEST_SEED = bytes(range(32))  # fixed, public: test vectors only


def c_array(name, data):
    rows = ", ".join(f"0x{b:02x}" for b in data)
    return f"static const uint8_t {name}[{len(data)}] = {{{rows}}};\n"


def vectors_json(out):
    """Fixture for the web updater's tests (blinky-console): one valid
    manifest with its image, signed with the same public test key."""
    import base64, json
    key = Ed25519PrivateKey.from_private_bytes(TEST_SEED)
    image = bytes((i * 7 + 3) & 0xFF for i in range(4099))
    m = sign(unsigned_manifest(image, 215), key)
    Path(out).write_text(json.dumps({
        "comment": "GENERATED by tools/ota_manifest.py vectors - public test key, never a release key",
        "pubkey": public_hex(key),
        "build": 215,
        "imageBase64": base64.b64encode(image).decode(),
        "manifestHex": m.hex(),
    }, indent=2) + "\n", encoding="utf-8", newline="\n")


def vectors(out):
    key = Ed25519PrivateKey.from_private_bytes(TEST_SEED)
    pub = bytes.fromhex(public_hex(key))
    image = bytes((i * 7 + 3) & 0xFF for i in range(4099))  # not a multiple of 4
    cur = 214
    cases = []

    def add(name, manifest, verdict):
        cases.append((name, manifest, verdict))

    good = sign(unsigned_manifest(image, 215), key)
    add("valid_upgrade", good, "OK")
    tampered = bytearray(good)
    tampered[12] ^= 0x01  # build field changed after signing
    add("tampered_after_signing", bytes(tampered), "BAD_SIGNATURE")
    other = Ed25519PrivateKey.from_private_bytes(bytes(31) + b"\x01")
    add("signed_by_other_key", sign(unsigned_manifest(image, 215), other), "BAD_SIGNATURE")
    add("wrong_board", sign(unsigned_manifest(image, 215, board=2), key), "WRONG_BOARD")
    add("same_build", sign(unsigned_manifest(image, cur), key), "NOT_NEWER")
    add("older_build", sign(unsigned_manifest(image, 200), key), "NOT_NEWER")
    add("emergency_rollback", sign(unsigned_manifest(image, 200, flags=FLAG_EMERGENCY_ROLLBACK), key), "OK")
    add("unknown_flag", sign(unsigned_manifest(image, 215, flags=0x0002), key), "UNKNOWN_FLAGS")
    add("bad_load_addr", sign(unsigned_manifest(image, 215, load_addr=0x26000), key), "BAD_LOAD_ADDR")
    bad_magic = bytearray(good)
    bad_magic[0] ^= 0xFF
    add("bad_magic", bytes(bad_magic), "BAD_MAGIC")

    lines = [
        "// GENERATED by tools/ota_manifest.py vectors - do not edit.\n",
        "// Signed with a fixed, PUBLIC test key (TEST_SEED); never a release key.\n",
        "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n",
        f"static const uint32_t VEC_CURRENT_BUILD = {cur};\n",
        c_array("VEC_PUBKEY", pub),
        c_array("VEC_IMAGE", image),
    ]
    for name, m, _ in cases:
        lines.append(c_array(f"VEC_{name.upper()}", m))
    lines.append("\nstruct VecCase { const char* name; const uint8_t* manifest; const char* verdict; };\n")
    lines.append("static const VecCase VEC_CASES[] = {\n")
    for name, _, verdict in cases:
        lines.append(f'    {{"{name}", VEC_{name.upper()}, "{verdict}"}},\n')
    lines.append("};\n")
    Path(out).write_text("".join(lines), encoding="utf-8", newline="\n")
    return len(cases)


# --------------------------------------------------------------------- cli ---

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sign")
    s.add_argument("--hex", required=True)
    s.add_argument("--build", required=True, type=int)
    s.add_argument("--out-dir", required=True)
    s.add_argument("--key-env")
    s.add_argument("--key-file")
    s.add_argument("--emergency-rollback", action="store_true",
                   help="allow installing over a newer build (signed flag)")
    v = sub.add_parser("verify")
    v.add_argument("--manifest", required=True)
    v.add_argument("--bin", required=True)
    v.add_argument("--pubkey", required=True, help="Ed25519 public key, 64 hex chars")
    v.add_argument("--current-build", type=int)
    g = sub.add_parser("vectors")
    g.add_argument("--out", required=True)
    g.add_argument("--json-out", help="also write the web updater's JSON fixture here")
    args = ap.parse_args()
    try:
        if args.cmd == "sign":
            key = load_key(args.key_env, args.key_file)
            start, image = hex_to_image(Path(args.hex).read_text(encoding="ascii"))
            if start != APP_LOAD_ADDR:
                raise OtaError(f"hex starts at 0x{start:X}, expected the application start 0x{APP_LOAD_ADDR:X}")
            flags = FLAG_EMERGENCY_ROLLBACK if args.emergency_rollback else 0
            manifest = sign(unsigned_manifest(image, args.build, flags=flags), key)
            verify(manifest, bytes.fromhex(public_hex(key)), image)
            out = Path(args.out_dir)
            out.mkdir(parents=True, exist_ok=True)
            (out / f"blinky-things-b{args.build}.bin").write_bytes(image)
            (out / f"blinky-things-b{args.build}.otam").write_bytes(manifest)
            print(f"signed b{args.build}: {len(image)} bytes, key {public_hex(key)[:16]}..., flags 0x{flags:04X}")
        elif args.cmd == "verify":
            f = verify(Path(args.manifest).read_bytes(), bytes.fromhex(args.pubkey),
                       Path(args.bin).read_bytes(), args.current_build)
            print(f"OK: build {f['build']}, {f['size']} bytes, flags 0x{f['flags']:04X}")
        elif args.cmd == "vectors":
            print(f"wrote {vectors(args.out)} cases to {args.out}")
            if args.json_out:
                vectors_json(args.json_out)
                print(f"wrote {args.json_out}")
    except (OtaError, OSError, ValueError) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
