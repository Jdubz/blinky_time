"""Golden UF2 fixture for the web flasher's UF2 checks: a synthetic app image
converted by uf2conv.py exactly as release packaging runs it, and accepted by
tools/uf2_upload.py validate_uf2.

Regenerate from the repo root (needs the Seeeduino nRF52 core for uf2conv.py):
  F=blinky-console/src/services/firmware/__fixtures__; python $F/make_fixtures.py $F
"""
import base64, json, random, subprocess, sys, tempfile
from pathlib import Path
sys.path.insert(0, "tools")
import uf2_upload

out = Path(sys.argv[1])
rng = random.Random(1234)

def rec(addr16, rtype, data):
    b = bytes([len(data), addr16 >> 8, addr16 & 0xFF, rtype]) + data
    return ":" + (b + bytes([(-sum(b)) & 0xFF])).hex().upper()

# 0x27000 .. 0x283A0 with a short gap at 0x27800..0x27840
segments = [(0x27000, bytes(rng.randrange(256) for _ in range(0x800))),
            (0x27840, bytes(rng.randrange(256) for _ in range(0xB5D)))]
lines = [rec(0, 0x04, bytes([0x00, 0x02]))]
for base, data in segments:
    for i in range(0, len(data), 16):
        lines.append(rec((base + i) & 0xFFFF, 0x00, data[i:i + 16]))
lines.append(rec(0, 0x01, b""))

tmp = Path(tempfile.mkdtemp())
(tmp / "app.hex").write_text("\r\n".join(lines) + "\r\n", newline="")
subprocess.run([sys.executable, str(uf2_upload.UF2CONV_PATH), str(tmp / "app.hex"),
                "-f", uf2_upload.UF2_FAMILY_ID, "-c", "-o", str(tmp / "app.uf2")],
               check=True, capture_output=True)
assert uf2_upload.validate_uf2(tmp / "app.uf2")
uf2 = (tmp / "app.uf2").read_bytes()
(out / "golden.json").write_text(json.dumps({
    "appStart": segments[0][0],
    "appUf2Base64": base64.b64encode(uf2).decode(),
}, indent=2) + "\n")
print("ok", len(uf2))
