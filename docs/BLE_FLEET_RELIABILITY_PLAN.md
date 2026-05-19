# BLE Fleet-Command Reliability Plan

Roadmap for closing the remaining gaps in BLE fleet-command delivery. Each
item is an independent change with its own scope, expected impact, and file
list. Pick them up in priority order; each lands a measurable improvement
on its own without depending on later items.

## What's already shipped (May 18, 2026)

Two server-side fixes are in production:

- **5× re-emit per command** (commit `65ef55a9` —
  `blinky_server/ble/advertiser.py`). Each `broadcast_command` emits the
  same payload 5 times with distinct sequence numbers
  (`COMMAND_REEMIT_COUNT=5`, `COMMAND_REEMIT_HOLD_MS=250`). Firmware
  applies each accepted emit; idempotent commands (`gen`, `effect`,
  `set`, `save`, `load`) tolerate up to 5 applications. Measured impact:
  cart_inner BLE `packets_rx` per 3-command burst went from `+1` (pre-
  fix, ~17% per-emit reception) to `+9` (~60%).

- **`MinInterval=50` / `MaxInterval=100`** D-Bus properties on the
  advertisement (commit `a8294f31` — same file). BlueZ default is
  1280 ms; without these the radio actually emits ~once per second
  even when we'd "set the payload." Declared as a property so BlueZ
  knows the operator's preferred adv rate. **Caveat:** BlueZ may
  silently clamp depending on kernel / mgmt-api version (bluez#314,
  bluez#833); declaring is defensive. Hardware A/B was inconclusive
  — receiver-side bottlenecks dominate.

## Measured baseline (cart_inner, May 18 bench)

Firmware-side BLE scanner diagnostics from a healthy cart after ~22 ks
uptime + the May 18 test run:

    [BLE] packets_rx=44 duped=6762 dropped=37 last_rssi=-46dBm

- **`packets_rx`** = unique `(source, sequence)` packets accepted into
  the firmware rxBuffer. Grew by ~30 across the day's tests.
- **`duped`** = same `(source, sequence)` seen again and discarded.
  Huge — BlueZ broadcasts each payload many times per on-air window;
  firmware dedups all but the first.
- **`dropped`** = packets with a fresh sequence that arrived while
  the rxBuffer's single slot was already full. THIS is the bottleneck
  we have to fix to get the next reliability tier — see item 1 below.

## Roadmap (priority order)

### 1. Multi-slot drop-oldest rxBuffer (firmware)

**Status:** ✅ SHIPPED in PR #144 (commit `9469b376`). Implemented in
`BleScanner.cpp/h` with 8-slot ring + per-(source, seq) dedup. The
notes below are kept as design rationale; the code lives in firmware.

**Scope:** `blinky-things/comms/BleScanner.cpp` + `.h`. Replace the
single-slot `rxBuffer_` / `rxReady_` flag with a small ring buffer (4-8
slots), drop-oldest on overrun. `update()` drains all ready slots per
main-loop tick. Keep `(source, seq)` dedup at INSERT time so a 5×
re-emit cycle doesn't enqueue 5 duplicates.

**Why it matters:** the current single-slot design loses any second
packet that arrives before `update()` runs. Main-loop iteration time
spikes to tens of milliseconds during heavy audio NN inference, so the
5× re-emit's 250-ms slots ARE wide enough to overlap a busy main loop.
Going from 1 slot to 4 reduces drop probability from "if the loop is
busy at the instant a packet arrives" to "if the loop is busy for the
entire ring-fill duration." Order-of-magnitude better.

**Expected impact:** `packets_rx` per fleet command goes from ~3-of-5
emits (current best) to ~5-of-5 emits in the common case; close to
100% per-command reception in normal conditions, even when the main
loop is under audio load.

**Out-of-scope hazard:** none — the dedup contract is preserved
because dedup happens at insert, not at consume.

### 2. Command-id idempotency token (protocol)

**Status:** ✅ SHIPPED in PR #144 (commits `f40ecd7d`, `c1e073fc`,
`fe56629e`). Implemented as `COMMAND_V2` packet type (0x04) with a
2-byte LE command_id token after the header. Firmware uses a global
ring of recent cmd_ids (not per-source — see [[project-bluez-addr-rotation]]
for why). Design rationale below kept as historical context.

**Original design notes:** definite improvement, replaces the 5x re-emit hack with
a cleaner contract.

**Scope:** `blinky_server/ble/protocol.py` +
`blinky-things/comms/BleProtocol.h` (header layout). Add a 16-bit
monotonic `command_id` field to the COMMAND packet header — separate
from the BLE-level `sequence` byte. The server's
`broadcast_command()` increments `command_id` once per LOGICAL
command and re-uses it across all 5 (or N) re-emits. The firmware
tracks the last `command_id` per source and short-circuits
re-applications: same command_id = no-op, even if the seq is fresh.

**Why it matters:** today's contract is "the firmware applies every
accepted packet; idempotent commands tolerate duplicates." This
works for `gen` / `effect` / `set` but is wasteful for `save`
(extra flash writes), incorrect for any future stateful command
(`inc`, `toggle`), and noisy on the serial console (multiple "OK"
echoes per fleet command). Command-id idempotency makes the
re-emit truly transparent.

**Expected impact:** safe to bump re-emit count to 10× or 20× without
worry. Future-proofs the protocol for non-idempotent commands. Removes
the "duplicate `OK effect: HueRotation\nOK effect: HueRotation`"
artifact in serial response output that we observed during the May
18 debug.

**Compatibility:** old firmware ignores the new field (it's
appended); server can keep emitting the legacy form for transition.
A device with new firmware sees a missing-field server as "no
idempotency token, fall back to seq-only dedup."

### 3. Per-source seq ring (firmware)

**Status:** ✅ SHIPPED in PR #144 (commit `9469b376`, same change as
item #1). The seen-ring stores last 8 (src, seq) tuples; design notes
kept as rationale.

**Scope:** `BleScanner.cpp` `lastSequence_` + `lastSourceAddr_`
become a tiny ring of the last 4-8 `(source, seq)` tuples. Looking
up "have I seen this exact tuple?" stays O(N), N small.

**Why it matters:** today the firmware remembers ONE sequence per
source. If two sources interleave (the broadcaster + another
broadcaster nearby + the device's own NUS chatter from a neighboring
device's BLE adv), legitimate fresh packets can be wrongly deduped.
Empirically the `duped` counter is enormous; a fraction of those may
be false dedups. Hard to quantify without instrumentation but trivial
to fix.

**Expected impact:** modest — closes a false-dedup tail risk that
shows up in noisy RF environments (festival deployment, many devices
in a small space). Worth doing once we touch `BleScanner.cpp` for #1.

### 4. Dedicated nRF52840 broadcaster dongle (hardware experiment)

**Status:** would-experiment.

**Scope:** add a USB-connected nRF52840 Dongle (PCA10059, ~$10) running
a ~50-line firmware that exposes a serial command "advertise these
bytes" and a "stop advertising" command. The Pi's `FleetBroadcaster`
sends commands over USB serial to the dongle instead of via the
local BlueZ adapter; BlueZ on the Pi keeps doing scan + GATT
central for BLE-DFU.

**Why it matters:** the Pi's BCM43455 is a single radio handling
adv + scan + central concurrently. The S140/BlueZ schedulers time-
multiplex them but the advertiser gets bumped when GATT central
(BLE-DFU flash) is active. A dedicated broadcaster gives full
duty-cycle to adv emissions and frees the BCM43455 for scan/central.

**Expected impact:** unknown without measurement. Probably small in
steady-state (the BCM43455 is rarely doing concurrent BLE-DFU);
meaningful during deploys when several BLE-DFU flashes might run
back-to-back. Worth measuring on-air emission rate with `btmon` on
the Pi vs the dongle to see if the BCM43455 is actually losing
emissions.

**Risk:** another bit of hardware to provision + ship-with. Don't
land this without measuring the win first.

### 5. Inverse-ACK gossip via NUS advertising

**Status:** ✅ SHIPPED. Phase 1 (firmware: each device exposes its
last accepted COMMAND_V2 `command_id` in scan-response manufacturer
data) landed in commit `699c3025`. Phase 2 (server: discovery
extracts the ACK, FleetBroadcaster re-emits the cached last command
for any laggard) lands in this PR. Per-device retry capped at
`REBROADCAST_MAX_ATTEMPTS = 3` and resets on every fresh
`broadcast_command`. Field validation pending.

**Scope:** each device's BLE advertisement already carries
manufacturer-data via NUS. Piggyback `last_applied_command_id` as a
small (2-byte) field. The Pi's discovery scanner already runs every
60 s — extend it to extract this field per device. If any device's
value lags the broadcaster's most-recent `command_id`, re-broadcast
the corresponding command.

**Why it matters:** gossipy reliability for the cases where a
specific device misses every emit of a specific command. Today such
a miss is permanent until the next command updates the device's
state. The gossip closes the loop without adding per-device GATT
connections (which we can't afford — BCM43455 is single-central).

**Expected impact:** unknown; depends on observed miss-rate after
items 1-3 land. May be redundant if 1+2 push reception to >99%.

**Risk:** adds scanner-side complexity. Don't land until 1-3 show
diminishing returns.

### Skipped: BLE Mesh

Researched and rejected. S140 supports Mesh in principle but
Adafruit Bluefruit and the Nordic Mesh SDK have incompatible
SoftDevice usage patterns: "while the SoftDevice is advertising the
Mesh won't be able to advertise" + "Mesh cannot receive packets
while SoftDevice is scanning" (Nordic DevZone confirmation). Mesh
provisioning + addressing is overkill for stateless idempotent
fan-out. The 5× re-emit + items 1-3 above achieve the same
reliability target with a fraction of the rewrite cost.

## Validating each landing

Each item should be measured against the bench setup before/after:

1. Note pre-change firmware diagnostics:
   `curl -s -X POST http://localhost:8420/api/devices/<sn>/command -H 'X-API-Key: …' -d '{"command":"ble"}'`
2. Fire a 5-command burst with 3 s gaps (`fire`, `water`, `plasma`,
   `audio`, `fire`).
3. Note post-change diagnostics. Compare `packets_rx`, `dropped`,
   `duped` deltas.
4. Verify the LAST commanded generator landed on EVERY device
   (carts via `POST /api/devices/<id>/command {"command":"gen"}`,
   big_bucket via direct NUS query — see `/tmp/big_bucket_query.py`
   from the May 18 debug session for the pattern).

The user-visible success criterion is that every fleet command
applies to every device within a single 5-command burst, with no
device missing any of the 5 generators. Items 1–3 shipped in PR #144
(May 18 2026); item 5 (gossip-ACK + re-broadcast) shipped 2026-05-19.
Item 4 (dedicated broadcaster dongle) remains an optional experiment
should remaining miss-rate after item 5 warrant it.
