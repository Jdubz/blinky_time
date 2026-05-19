# Postmortem: cart_inner / cart_outer configured-boot crash → BLE-DFU loop (2026-05-18)

## Summary

Both LemonCart devices (cart_inner, cart_outer) crash-looped into BLE-DFU
recovery on every attempt to push their stored device config, eventually
landing in safe mode after the firmware's `RebootFrequencyCounter`
quarantined the config at the 5-trip threshold. Affected window: from
firmware commit `752ebc16` (2026-05-17, "Address PR 142 review feedback")
through `0270ca3f` (2026-05-18, this postmortem's fix).

User-visible impact: the carts were unreachable in their normal
configured mode. Safe mode left audio + BLE active but LEDs dark, so the
fleet appeared "alive but doing nothing." The same config that worked
fine on b166 (the prior fleet build) crashed reliably on b167+.

Root cause: **a typo in two registry JSON files** (`ledType: 12390`) that
the firmware silently tolerated for ~2 months and finally rejected when
the LED driver gained correct buffer-overflow validation in PR 142.

## The bug

`devices/registry/cart_inner.json` and `devices/registry/cart_outer.json`
both shipped with:

    "ledType": 12390

`12390 = 0x3066`. The firmware's `Nrf52PwmLedStrip` constructor decodes
the lower byte (`0x66 = 0b01100110`) into three 2-bit fields:

    bOffset = (0x66 >> 0) & 0x3 = 0b10 = 2
    gOffset = (0x66 >> 2) & 0x3 = 0b01 = 1
    rOffset = (0x66 >> 4) & 0x3 = 0b10 = 2

**r and b are both offset 2** — i.e., every pixel write puts both the
red value and the blue value into the same byte of the 3-byte pixel
buffer, with the second write silently overwriting the first.

`devices/registry/README.md` claimed `12390 = NEO_GRB + NEO_KHZ800`,
which is mathematically wrong: `NEO_GRB = 0x52 = 82` and `NEO_KHZ800 = 0`,
so `NEO_GRB + NEO_KHZ800 = 82`, not 12390. The 12390 value was a typo
that landed in commit `0a20ce5b` (CONFIG_VERSION v28 introduction,
March 2026) and never got cross-checked against the NEO_* constants.

The fleet-wide table tells the story: every other device's registry
uses `82` (NEO_GRB, WS2812B's standard byte order) or `6` (NEO_RGB,
big_bucket's native-RGB strip). Only the two cart files had `12390`.

## The regression

PR 142 (commit `752ebc16`) fixed a buffer-overflow class of bug in
`Nrf52PwmLedStrip`:

> HIGH: Nrf52PwmLedStrip buffer overflow on offset==3. Validate decoded
> R/G/B byte offsets ≤ 2 and distinct at construction; on bad ledType,
> log ERROR and leave buffers unallocated so isValid()→false trips the
> caller's existing haltWithError path.

That validation is correct. The pre-fix code wrote to byte offsets
without checking distinctness, so duplicate-offset configs produced
visibly-wrong colors but no crash — neither cart was actually
displaying correct color since v28. The fix raised the floor: bad
configs now fail loud at construction.

But "fail loud at construction" feeds into `setup()`'s existing
`haltWithError` path:

    if (!asyncStrip || !asyncStrip->isValid()) {
      haltWithError(F("ERROR: Async LED strip allocation failed"));
    }

`haltWithError` is `while(1) { delay(10000); }`. `delay()` does NOT
feed the hardware watchdog. WDT fires at 15 s, the device resets,
`SafeBootWatchdog` increments its GPREGRET2 boot counter, and after
3 cycles the counter hits the BLE-DFU recovery threshold. Five further
crash-counter trips fire `RebootFrequencyCounter::quarantineDeviceConfig`,
which marks the stored config invalid; the device boots safe mode next
power-up because there's no valid config to load.

Net behaviour: every attempt to push a fresh `cart_inner` or `cart_outer`
config crashed the device into safe mode after ~5 minutes of crash
cycling. The "fix" — re-pushing the config — re-triggered the same
loop.

## Why diagnosing this took longer than it should

I burned several hours on instrumentation that was looking in the wrong
place:

1. **Hypothesised WDT timeout from slow setup()**. Doubled the WDT to
   60 s. Still crashed at the same rate. Should have been the first
   signal that "WDT-due-to-slowness" was wrong; instead I kept piling
   on instrumentation.

2. **Tried to capture serial output during the crash window**. USB CDC
   output during USB re-enumeration is unreliable on this BSP — bytes
   printed in the first ~1 s after `Serial.begin()` get lost. Every
   marker I added to `setup()` was invisible on the host.

3. **Built a persistent-RAM boot-phase trace mechanism** to work around
   the lost-serial problem. The first attempt used
   `__attribute__((section(".noinit")))`, which the Adafruit nRF52
   linker doesn't honour (it has no explicit `.noinit` section, so
   GCC orphans the variables between `.data` and `.bss`, and the
   startup data-copy loop wipes them every boot). The second attempt
   used a fixed RAM address at `0x2003F000`. This worked, but the
   trace it captured was `crash-line: 313` (the marker right after
   `configStorage.begin`). I initially misread this as "crash in
   `RebootFrequencyCounter::checkAndIncrement`" — but that function
   was the same code safe mode also runs without issue.

What finally cracked it: stepping back, listing the seven commits
between the known-working b166 and the broken HEAD, noticing PR 142
touched `Nrf52PwmLedStrip.cpp`, reading the diff, and decoding 12390
by hand — at which point r=b=2 is immediately obvious. The
instrumentation had been correctly telling me the device made it deep
into setup before crashing; the trace was a red herring because the
crash isn't a hard fault, it's the WDT-with-loud-rationale firing from
`haltWithError`'s spin loop.

## The fix

[`0270ca3f`](https://github.com/Jdubz/blinky_time/commit/0270ca3f):

- `devices/registry/cart_inner.json`: `ledType: 12390` → `82`
- `devices/registry/cart_outer.json`: `ledType: 12390` → `82`
- `devices/registry/README.md`: removed the false "12390 = NEO_GRB + NEO_KHZ800"
  claim; added an explicit "do NOT use 12390" note with the bit-math.

Verified post-fix: both carts boot configured-mode cleanly on first try.
cart_outer at 96 LEDs single-strand, cart_inner at 104 LEDs in
`ledMode: "multistrand"`, both with `prevBoot: "completed"` and FPS in
the 180-210 range.

## Protections (this commit)

Three layers, defense in depth:

1. **Firmware-side validation at upload + flash-load**
   (`blinky-things/config/DeviceConfigLoader.cpp::validate`). Mirrors the
   `Nrf52PwmLedStrip` ctor rule — `ledType` lower byte must decode to
   r/g/b offsets that are all ≤ 2 and pairwise distinct. Catches a bad
   config two ways:
     - `uploadDeviceConfig` runs `validate()` before persisting → bad
       `device upload` rejected with a clear `[WARN]` message; nothing
       hits flash.
     - `DeviceConfigLoader::loadFromFlash` runs `validate()` at boot →
       if a bad config somehow got persisted (older firmware, manual
       flash, future regression), `loadFromFlash` returns false and the
       firmware enters safe mode instead of haltWithError-looping into
       BLE-DFU.

2. **Server-side CI test for registry JSONs**
   (`blinky-server/tests/test_registry_jsons.py`). Parametrised test
   that opens every `devices/registry/*.json` and decodes its `ledType`
   using the same bit-math the firmware uses. Pins the rule for any
   new device added to the registry — a "next 12390" typo fails
   pre-merge instead of mid-deploy.

3. **WDT feeds at the genuinely-slow setup() phases**
   (`blinky-things.ino`). The original `setup()` had a single feed in
   `SafeBootWatchdog::begin()` and nothing else until the main loop —
   making the entire init path one big 15-second budget. The fix adds
   `SafeBootWatchdog::feed()` after the four known-slow phases (audio
   begin / RenderPipeline init / Bluefruit begin / bleDfu begin) and
   splits the 3-second LED test delay into three 1-second chunks with
   feeds. Together these mean a slow individual phase (BLE init under
   RF noise, a slow flash read, an NN tensor arena alloc) can't trip
   the WDT on its own — the WDT can only fire if a SINGLE phase
   exceeds 15 s of CPU time, which is a much stronger guarantee.

## Lessons

- **README claims must be tested.** The "12390 = NEO_GRB + NEO_KHZ800"
  line in `devices/registry/README.md` was wrong, sat there for 2
  months, and was the source of authority that operators used to copy
  the value into new configs. CI now lints the actual bit-math, not
  the prose.
- **"Validation found a bug" is a bug REPORT, not a bug.** When PR 142
  added the offset-distinctness check, the test fleet had bad configs
  that the new validation correctly flagged. The validation wasn't the
  regression; the configs were. A pre-merge fleet smoke test (push
  every registry config to a representative device and assert it
  reaches loop()) would have caught this.
- **When time-budgeted instrumentation contradicts the hypothesis,
  abandon the hypothesis early.** I bumped the WDT to 60 s and the
  device still failed at the same rate; that ruled out "WDT-too-tight"
  in one experiment but I kept investigating from the same angle for
  another hour. The correct response would have been to step back and
  look at the regression window first.
- **`haltWithError(while(1) delay)` is a confusing failure mode**
  because it tripwires the WDT into looking like a slow-setup crash.
  Worth considering whether `haltWithError` should explicitly enter
  BLE-DFU recovery instead of waiting for the WDT to do it indirectly
  — operators get clearer "bad config" signal that way. Filed as
  follow-up.

## Affected commits

- Regression introduced: `0a20ce5b` (CONFIG_VERSION v28, March 2026) —
  the `ledType: 12390` typo originates here.
- Latent bug exposed: `752ebc16` (PR 142, 2026-05-17) — added correct
  `Nrf52PwmLedStrip` ctor validation.
- Fix: `0270ca3f` (this commit's predecessor) — registry change + the
  defensive `validate()` extension lives in the commit alongside this
  doc.
