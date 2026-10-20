"""Golden fixtures for the web flasher's TS ports, produced by the Python
implementations they mirror (compile.crc16 / generate_dfu_package layout,
uf2conv.py as release packaging runs it).

Regenerate from the repo root (needs the Seeeduino nRF52 core for uf2conv.py):
  F=blinky-console/src/services/firmware/__fixtures__; python $F/make_fixtures.py $F
"""
import base64, json, random, struct, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, "tools"); sys.path.insert(0, "blinky-server")
import uf2_upload
from blinky_server.firmware.compile import crc16

out = Path(sys.argv[1])
rng = random.Random(1234)

def rec(addr16, rtype, data):
    b = bytes([len(data), addr16 >> 8, addr16 & 0xFF, rtype]) + data
    return ":" + (b + bytes([(-sum(b)) & 0xFF])).hex().upper()

# 0x27000 .. ~0x283A0 with a hole at 0x27800..0x27840 and a 64 KiB boundary-free layout
segments = [(0x27000, bytes(rng.randrange(256) for _ in range(0x800))),
            (0x27840, bytes(rng.randrange(256) for _ in range(0xB5D)))]
lines = [rec(0, 0x04, bytes([0x00, 0x02]))]
for base, data in segments:
    for i in range(0, len(data), 16):
        a = base + i
        lines.append(rec(a & 0xFFFF, 0x00, data[i:i + 16]))
lines.append(rec(0, 0x05, struct.pack(">I", 0x27001)))
lines.append(rec(0, 0x01, b""))
hex_text = "\r\n".join(lines) + "\r\n"
tmp = Path(tempfile.mkdtemp())
(tmp / "app.hex").write_text(hex_text, newline="")

start = segments[0][0]; end = segments[-1][0] + len(segments[-1][1])
bin_ = bytearray(end - start)
for base, data in segments:
    bin_[base - start: base - start + len(data)] = data
crc = crc16(bytes(bin_))
init = struct.pack("<HHIHH", 0x0052, 0xFFFF, 0xFFFFFFFF, 1, 0xFFFE) + struct.pack("<H", crc)

subprocess.run([sys.executable, str(uf2_upload.UF2CONV_PATH), str(tmp / "app.hex"),
                "-f", uf2_upload.UF2_FAMILY_ID, "-c", "-o", str(tmp / "app.uf2")],
               check=True, capture_output=True)
assert uf2_upload.validate_uf2(tmp / "app.uf2")
uf2 = (tmp / "app.uf2").read_bytes()

vectors = {}
for name, data in {"empty": b"", "ascii": b"123456789", "bytes256": bytes(range(256))}.items():
    vectors[name] = {"hex": data.hex(), "crc": crc16(data)}
(out / "golden.json").write_text(json.dumps({
    "start": start, "end": end, "binLength": len(bin_), "crc16": crc,
    "initPacket": init.hex(), "crcVectors": vectors,
    "appHex": hex_text, "appUf2Base64": base64.b64encode(uf2).decode(),
}, indent=2) + "\n")
print("ok", start, end, hex(crc), len(uf2))
