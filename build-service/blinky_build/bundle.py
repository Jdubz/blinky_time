"""Read a git bundle's header: its prerequisites and refs (gitformat-bundle(5)).

Parsed here, not from `git bundle verify` output, so a missing prerequisite (409) is told
apart from a malformed bundle (400) without reading human-readable messages.
"""

import re

OID_RE = re.compile(r"^[0-9a-f]{40}$")
# What a bundle ref may be called before it is mapped to refs/dev/<id>/<n>.
REF_RE = re.compile(r"^(HEAD|refs/[A-Za-z0-9._/-]+)$")
MAX_HEADER = 1 << 20


class BundleError(Exception):
    pass


class Bundle:
    def __init__(self, version, prerequisites, refs):
        self.version = version
        self.prerequisites = prerequisites  # [oid]
        self.refs = refs  # [(oid, refname)]


def parse_header(path):
    with open(path, "rb") as f:
        head = f.read(MAX_HEADER)
    end = head.find(b"\n\n")
    if end < 0:
        raise BundleError("not a git bundle (no header end in the first MiB)")
    try:
        lines = head[:end].decode("utf-8").split("\n")
    except UnicodeDecodeError:
        raise BundleError("not a git bundle (header is not text)") from None
    sig = lines[0]
    if sig == "# v2 git bundle":
        version = 2
    elif sig == "# v3 git bundle":
        version = 3
    else:
        raise BundleError(f"not a git bundle (signature {sig[:40]!r})")
    prereqs, refs = [], []
    for line in lines[1:]:
        if line.startswith("@"):
            if version != 3:
                raise BundleError("capability line in a v2 bundle")
            key, _, value = line[1:].partition("=")
            if key == "object-format":
                if value != "sha1":
                    raise BundleError(f"unsupported object format {value!r}")
            else:
                # @filter (a partial bundle) or anything newer: refused, not guessed at.
                raise BundleError(f"unsupported bundle capability {key!r}")
        elif line.startswith("-"):
            oid = line[1:].split(" ", 1)[0]
            if not OID_RE.match(oid):
                raise BundleError(f"bad prerequisite line {line[:60]!r}")
            prereqs.append(oid)
        else:
            oid, _, name = line.partition(" ")
            if not OID_RE.match(oid) or not REF_RE.match(name) or ".." in name:
                raise BundleError(f"bad ref line {line[:80]!r}")
            refs.append((oid, name))
    if not refs:
        raise BundleError("bundle has no refs")
    return Bundle(version, prereqs, refs)
