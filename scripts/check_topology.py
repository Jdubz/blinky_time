#!/usr/bin/env python3
"""Fail on network topology in tracked files: this repo is public.

Topology (host names, private and tailnet addresses, mDNS names) lives only in private config
outside the repo (~/.config/blinky/*.env, keys listed in the docs). This check keeps it from
creeping back. Stdlib only; the same script runs in blinky_time, blinky-firmware and blinky-corpus.

Rules:
  name     a known machine name. They're listed as SHA-256 prefixes, so the check doesn't
           publish the names it looks for.
  private  a host named in your own ~/.config/blinky/*.env (local runs only; CI has no such file).
  mdns     an mDNS name (<host>.local).
  tailnet  a tailnet name (*.ts.net).
  address  a private or tailnet IPv4 address (10/8, 172.16/12, 192.168/16, 100.64/10).
           Use the documentation ranges instead (192.0.2.x, 198.51.100.x, 203.0.113.x) and
           example.invalid for names.

Findings print the offending line locally; in CI (CI set) only path, line and rule, because CI
logs are public. Exceptions go in .topology-allowlist at the repo root, one per line:
    <path glob> <regex matched against the offending line>
with a comment above each saying why. Usage: python3 scripts/check_topology.py [paths...]
"""

from __future__ import annotations

import fnmatch
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlsplit

# sha256(name)[:16] of each known machine name, lowercase.
KNOWN_NAME_HASHES = {
    "91959b1c8e7722ef",
    "c4f0272551521495",
    "6f03b6d7ddb67d37",
    "a1bfdbceef5f0ea2",
    "17ab02b3b79b41fc",
    "b98f0989c8a20774",
    "aeb053a307408520",
    "4f0095000f70c5b4",
}
NAME_LENGTHS = range(4, 32)

TOKEN = re.compile(r"[A-Za-z0-9]+(?:-[A-Za-z0-9]+)*")
PATTERNS = {
    # <host>.local, but not ~/.local, .env.local (Vite) or threading.local().
    "mdns": re.compile(r"\b(?!env\.local\b)[A-Za-z0-9][A-Za-z0-9-]*\.local\b(?!\s*\()", re.IGNORECASE),
    "tailnet": re.compile(r"\b[A-Za-z0-9-]+\.ts\.net\b", re.IGNORECASE),
    "address": re.compile(
        r"(?<![\w.])(?:10\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01])|192\.168|100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7]))"
        r"\.\d{1,3}\.\d{1,3}(?![\w.]*\d)"
    ),
}
ALLOWLIST = ".topology-allowlist"
PRIVATE_DIR = Path.home() / ".config" / "blinky"
LOCAL = {"localhost", "127.0.0.1", "0.0.0.0", "::1"}


def name_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()[:16]


def private_names() -> set[str]:
    """Hosts in the private config's values (URLs, user@host, host[:port]), for local runs.
    Secrets (keys named *KEY*, *SECRET*, *TOKEN*, *PASSWORD*) are never read as hosts."""
    names: set[str] = set()
    if not PRIVATE_DIR.is_dir():
        return names
    bare = re.compile(r"([A-Za-z0-9][A-Za-z0-9.-]*[A-Za-z0-9])(?::\d+)?")
    for f in sorted(PRIVATE_DIR.glob("*.env")):
        try:
            text = f.read_text()
        except OSError:
            continue
        for line in text.splitlines():
            if "=" not in line or line.lstrip().startswith("#"):
                continue
            key, value = (x.strip() for x in line.split("=", 1))
            if re.search(r"KEY|SECRET|TOKEN|PASSWORD", key, re.IGNORECASE):
                continue
            value = value.strip("'\"")
            if "://" in value:
                host = urlsplit(value).hostname or ""
            elif "@" in value:
                host = value.rsplit("@", 1)[1].split(":", 1)[0]
            elif (m := bare.fullmatch(value)) and (
                "." in m.group(1) or re.search(r"HOST|SSH|SERVER|ADDR|ENDPOINT", key, re.IGNORECASE)
            ):
                host = m.group(1)  # a dotted name or address, or any host[:port] in a host-valued key
            else:
                continue
            host = host.lower()
            if host and host not in LOCAL and not host.endswith((".invalid", ".example")):
                names.add(host)
                if not host.replace(".", "").isdigit() and len(host.split(".", 1)[0]) >= 5:
                    names.add(host.split(".", 1)[0])  # the bare name too: "box" of box.lan
    return names


def load_allowlist(root: Path) -> list[tuple[str, re.Pattern[str]]]:
    rules = []
    path = root / ALLOWLIST
    if not path.is_file():
        return rules
    for n, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            sys.exit(f"{ALLOWLIST}:{n}: expected '<path glob> <regex>'")
        rules.append((parts[0], re.compile(parts[1])))
    return rules


def tracked_files(root: Path) -> list[str]:
    out = subprocess.run(["git", "ls-files", "-z"], cwd=root, check=True, capture_output=True).stdout
    return [p for p in out.decode().split("\0") if p]


def plain_case(text: str) -> bool:
    """Host names are written lowercase (or Capitalized, or in caps); CamelCase is code."""
    return text.islower() or text.isupper() or (text[:1].isupper() and text[1:].islower())


def name_hits(line: str, private: set[str]) -> list[str]:
    hits = []
    for m in TOKEN.finditer(line):
        tok = m.group(0)
        if not plain_case(tok):
            continue
        parts = tok.lower().split("-")[:8]
        for i in range(len(parts)):
            for j in range(i + 1, len(parts) + 1):
                cand = "-".join(parts[i:j])
                if len(cand) in NAME_LENGTHS and name_hash(cand) in KNOWN_NAME_HASHES:
                    hits.append("name")
    for name in private:
        for m in re.finditer(rf"(?<![\w-]){re.escape(name)}(?![\w-])", line, re.IGNORECASE):
            if plain_case(m.group(0)):
                hits.append("private")
    return hits


def scan(root: Path, paths: list[str]) -> list[str]:
    allow = load_allowlist(root)
    private = private_names()
    findings = []
    for rel in paths:
        if rel == ALLOWLIST:
            continue
        p = root / rel
        try:
            data = p.read_bytes()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue
        for n, line in enumerate(data.decode("utf-8", "replace").splitlines(), 1):
            rules = name_hits(line, private)
            rules += [name for name, rx in PATTERNS.items() if rx.search(line)]
            if not rules:
                continue
            if any(fnmatch.fnmatch(rel, g) and rx.search(line) for g, rx in allow):
                continue
            # CI logs are public: never echo a finding there, nor a private name anywhere.
            if "private" in rules:
                shown = "<line withheld: names a private host>"
            elif os.environ.get("CI"):
                shown = "<line withheld in CI: run the check locally to see it>"
            else:
                shown = line.strip()[:160]
            findings.append(f"{rel}:{n}: [{','.join(sorted(set(rules)))}] {shown}")
    return findings


def main() -> int:
    root = Path(
        subprocess.run(["git", "rev-parse", "--show-toplevel"], check=True, capture_output=True, text=True).stdout.strip()
    )
    paths = sys.argv[1:] or tracked_files(root)
    paths = [os.path.relpath(Path(p).resolve(), root) if os.path.isabs(p) else p for p in paths]
    findings = scan(root, paths)
    for f in findings:
        print(f)
    if findings:
        print(
            f"\n{len(findings)} topology reference(s). The repo is public: host names and addresses go in "
            "~/.config/blinky/*.env (see the docs' private-config section), examples use 192.0.2.x or "
            f"example.invalid, and a justified exception goes in {ALLOWLIST} with a reason.",
            file=sys.stderr,
        )
        return 1
    print("no topology references")
    return 0


if __name__ == "__main__":
    sys.exit(main())
