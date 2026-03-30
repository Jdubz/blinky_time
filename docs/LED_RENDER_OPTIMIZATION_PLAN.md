# LED Render Optimization Plan

*Created: March 29, 2026. Rewritten: March 30, 2026.*

Target hardware: nRF52840 (Cortex-M4F, 64 MHz, 256 KB RAM). 32x32 = 1024 WS2812B LEDs.
Goal: determine if 1024-pixel display is feasible on nRF52840 and optimize accordingly.

---

## Feasibility Analysis

### Wire time (hard physics, unavoidable)

1024 LEDs x 3 bytes x 8 bits x 1.25 us/bit = **30.72 ms** + 300 us reset = **~31 ms per frame**.

This is the absolute floor for frame time — no software optimization can reduce it. It limits the theoretical maximum to **~32 FPS**.

### Adafruit NeoPixel on nRF52840: PWM + EasyDMA

The Adafruit library uses the nRF52840 PWM peripheral with EasyDMA for WS2812B timing. It does NOT reinstall a driver each frame (unlike the ESP32 `espShow()` path). However, it has two critical problems:

**Problem 1: 48 KB transient allocation per frame.**
`show()` converts the GRB pixel buffer into a 16-bit PWM duty-cycle pattern — one `uint16_t` per bit. For 1024 LEDs:
```
pattern_size = 3072 bytes * 8 bits * 2 bytes/uint16_t + 4 = 49,156 bytes (~48 KB)
```
This 48 KB buffer is **malloc'd and free'd every frame** via `rtos_malloc`/`rtos_free`. On a 256 KB device this is a heap fragmentation bomb. Even if total free memory is sufficient, fragmentation from repeated 48 KB alloc/free cycles will eventually cause malloc failure.

**Problem 2: show() blocks for the entire wire time (~31 ms).**
After starting the PWM sequence, `show()` polls `EVENTS_SEQEND[0]` in a tight loop with `yield()`. On the Seeed nRF52840 core, `yield()` gives FreeRTOS a chance to run BLE tasks, but `show()` itself does not return until transmission completes. No CPU work overlaps with wire time.

**Problem 3: PWM peripheral is disabled and disconnected after every frame.**
Lines 2376-2378 disable the PWM and disconnect the output pin after each transmission, requiring full re-setup next frame. Not as expensive as ESP32's `rmt_driver_install()`, but unnecessary overhead.

### RAM budget

| Component | Size | Notes |
|-----------|------|-------|
| PixelMatrix RGB[1024] | 3,072 | Generation target |
| Adafruit NeoPixel GRB[1024] | 3,072 | Output buffer |
| LEDMapper positionToIndex | 4,096 | `int[1024]` — shrinkable |
| LEDMapper indexToX | 4,096 | `int[1024]` — shrinkable |
| LEDMapper indexToY | 4,096 | `int[1024]` — shrinkable |
| **PWM pattern buffer (transient)** | **49,156** | **malloc'd every frame** |
| TFLite arena | 32,768 | Only 3,404 used (Conv1D W16) |
| FrameOnsetNN window buffer | ~3,328 | W16 x 52 features x float |
| SharedSpectralAnalysis | ~7,000 | FFT, mel, whitening buffers |
| Particle pools (4 generators) | 18,432 | 4 x 192 particles x 24 B |
| System/BLE/Serial/stack | ~30,000 | Estimate |
| **Peak total** | **~163 KB** | **64% of 256 KB** |

~93 KB headroom at peak. Feasible but tight — and the 48 KB transient alloc is the fragmentation risk.

**Easy wins already visible:**
- Right-size TFLite arena to 4 KB (actual usage 3,404): saves **28 KB**
- Shrink LEDMapper to uint16/uint8: saves **8 KB**
- Pre-allocate PWM buffer once: eliminates fragmentation risk

### CPU budget per frame (64 MHz)

| Stage | Estimated time | Notes |
|-------|---------------|-------|
| Audio (NN + FFT + ACF + PLP) | ~10 ms | NN inference alone is 6.8 ms |
| PixelMatrix clear | ~0.1 ms | 3 KB memset |
| Fire generation (192 particles) | ~2-3 ms | Physics + curl noise + splat |
| Effect (NoOp) | 0 ms | Pass-through |
| EffectRenderer copy | ~1 ms | 1024 pixels: lookup + setPixelColor |
| **CPU subtotal** | **~13-14 ms** | |
| show() blocking | ~31 ms | PWM + DMA, no overlap |
| **Total frame time** | **~44-45 ms** | |
| **Resulting FPS** | **~22 FPS** | |

With async DMA (CPU overlaps wire time): **~32 FPS** (wire-time limited).

### Verdict: feasible with custom LED driver

1024 LEDs on nRF52840 is feasible if:
1. The 48 KB PWM buffer is pre-allocated (not malloc'd per frame)
2. `show()` is made non-blocking so CPU work overlaps wire time
3. TFLite arena is right-sized to reclaim 28 KB

Without these changes, the stock Adafruit NeoPixel library will eventually malloc-fail on the 48 KB transient allocation due to heap fragmentation. The existing comment in `DeviceConfigLoader.cpp:155` ("nRF52840 practical ceiling is ~512 LEDs") reflects this — it's a library limitation, not a hardware one.

---

## Plan

### Phase 1 — Trivial wins (no architectural change)

**1a. `memset` in `PixelMatrix::clear()`**
Replace the `RGB(0,0,0)` loop with `memset(pixels_, 0, width_ * height_ * sizeof(RGB))`.
- Impact: negligible speed, architecturally correct.
- Risk: none. One line.

**1b. Eliminate Color()/setPixelColor pack-unpack**
`EffectRenderer::render()`: replace `leds_.setPixelColor(idx, leds_.Color(r,g,b))` with the three-argument form `leds_.setPixelColor(idx, r, g, b)`. Also fix `NeoPixelLedStrip::setPixelColor(index, r, g, b)` to call `strip_->setPixelColor(index, r, g, b)` directly instead of re-packing with `strip_->Color()`.
- Impact: eliminates 1,024 Color() calls + 2,048 virtual dispatches per frame. ~0.5 ms.
- Risk: none. Two files, ~5 lines.

**1c. Cache `numPixels()` outside the render loop**
`leds_.numPixels()` is a virtual call made once per pixel iteration. Call it once before the loop.
- Impact: eliminates 1,024 virtual dispatches per frame. ~0.2 ms.
- Risk: none. One line.

**1d. Skip effect call entirely when no effect is active**
In `RenderPipeline::render()`, guard the `currentEffect_->apply()` call:
```cpp
if (effectType_ != EffectType::NONE) {
    currentEffect_->apply(pixelMatrix_);
}
```
- Impact: eliminates virtual dispatch + function call on every frame when no effect active (the common case).
- Risk: none. ~5 lines.

**1e. Right-size TFLite arena**
Change `ARENA_SIZE` from 32,768 to 4,096 (actual usage is 3,404 bytes for Conv1D W16).
- Impact: **saves 28 KB RAM**. Critical headroom for 1024-LED configs.
- Risk: low. If a future larger model is deployed, arena size must increase to match. Add a `static_assert` or runtime check that `arena_used_bytes() < ARENA_SIZE`.

### Phase 2 — Memory optimizations

**2a. Shrink LEDMapper arrays — saves 8 KB**
`positionToIndex`: `int[N]` -> `uint16_t[N]` (max index 65535, sufficient for any device).
`indexToX`, `indexToY`: `int[N]` -> `uint8_t[N]` (max dimension 255).
Use `0xFFFF` as the invalid sentinel for `positionToIndex` instead of `-1`.
- Impact: **8 KB RAM saved** (12 KB -> 4 KB) for a 1024-LED device.
- Risk: low. Requires updating sentinel checks. ~30 lines across LEDMapper.h/.cpp.

**2b. EffectRenderer: iterate by LED index, write direct to Adafruit buffer**
Currently iterates (x,y) -> `positionToIndex[y*w+x]` lookup -> setPixelColor. Change to iterate `ledIndex` 0..N-1, read `matrix.getPixel(indexToX[i], indexToY[i])`, write GRB bytes directly into the Adafruit `getPixels()` buffer with inline brightness and GRB reordering. Writes to the output buffer become sequential (cache-friendly). Eliminates all virtual dispatch in the inner loop.
- Impact: eliminates `positionToIndex` lookup per pixel, eliminates all virtual dispatch in hot path. ~1-2 ms saved.
- Risk: low-medium. Requires `getPixels()` access from `NeoPixelLedStrip`. ~40 lines.
- Note: reads from PixelMatrix become random-access via indexToX/indexToY, but writes (the bottleneck for write-back cache) become sequential.

**2c. Pre-apply brightness once, disable per-pixel multiply**
Call `setBrightness(255)` (which Adafruit internally stores as 0 = "no scaling") during `begin()`. Apply the actual brightness value once during the EffectRenderer copy in 2b rather than letting Adafruit do it per-pixel. Eliminates 3,072 multiplies+shifts per frame.
- Impact: ~0.5 ms saved per frame.
- Risk: low. ~20 lines.

### Phase 3 — Custom nRF52840 LED driver (high impact)

**3a. `Nrf52PwmLedStrip` with persistent PWM + pre-allocated pattern buffer**

The Adafruit NeoPixel library's per-frame malloc/free of a 48 KB buffer is the primary reliability risk for 1024 LEDs on nRF52840. Replace it with a custom driver.

Implementation:
- Create `blinky-things/hal/hardware/Nrf52PwmLedStrip.h/.cpp` implementing `ILedStrip`
- `begin()`: allocate GRB pixel buffer (3 KB) + PWM pattern buffer (48 KB) once. Claim a PWM peripheral and leave it configured (no per-frame setup/teardown). Return false if malloc fails (device knows at boot it can't support this LED count).
- `show()`: convert GRB -> PWM pattern in-place, start DMA sequence, **return immediately**. Track state as `IDLE` / `TRANSMITTING`.
- `waitForShow()`: poll `EVENTS_SEQEND[0]` with `yield()`. Called at the start of the next `show()` to ensure the previous frame completed before overwriting the buffer.
- Conditional compile: `#ifdef BLINKY_PLATFORM_NRF52840`

**Why not double-buffer?** Double-buffering the PWM pattern would require 2 x 48 KB = 96 KB — too expensive on 256 KB. Instead, single-buffer with a `waitForShow()` fence: generation writes to the GRB pixel buffer freely (DMA reads from the separate PWM pattern buffer), then the next `show()` waits for DMA completion before converting GRB -> PWM and starting a new transmission.

**Frame pipeline with async show():**
```
Frame N:
  1. waitForShow()              // ensure frame N-1 DMA complete (~0 ms if generation > 31 ms, else blocks remainder)
  2. GRB -> PWM pattern         // ~1 ms conversion
  3. start DMA                  // returns in <100 us
  4. audio + generate frame N+1 // ~13 ms CPU work, overlaps with DMA wire time

Effective frame time: max(31 ms wire, 14 ms CPU) = 31 ms = ~32 FPS
```

- Impact: **eliminates 48 KB malloc/free per frame** (reliability), **overlaps 31 ms wire time with CPU** (~22 -> ~32 FPS), **eliminates PWM setup/teardown per frame**.
- Risk: medium. New driver, ~150 lines. Test on bare chip before flashing installed devices.
- Files: new `Nrf52PwmLedStrip.h/.cpp`, conditional in `blinky-things.ino`.

**3b. Alternative: I2S-based driver (smaller buffer)**

The nRF52840 I2S peripheral is an alternative to PWM for WS2812B driving. At an I2S bit rate of ~3.2 MHz (4 I2S bits per WS2812B bit), the pattern buffer shrinks to:
```
1024 LEDs x 3 bytes x 8 bits x 4 I2S bits / 8 = 12,288 bytes (~12 KB)
```
**12 KB vs 48 KB** — a significant RAM saving. I2S also has hardware double-buffered DMA pointers, making async operation more natural.

Trade-off: more complex setup (I2S peripheral configuration, clock source selection, sample format). The nRF52840 I2S peripheral requires a master clock which must be derived from HFCLK. WS2812B timing tolerance is forgiving (250-550 ns for T0H), so clock accuracy is not critical.

This is an alternative to 3a, not a prerequisite. If RAM headroom from Phase 1+2 is sufficient, PWM (3a) is simpler. If RAM is tight, I2S (3b) saves 36 KB.

- Impact: **saves 36 KB RAM** vs PWM pattern buffer, same async benefits.
- Risk: medium-high. I2S configuration is more involved. ~200 lines.

---

## Summary

| # | Change | RAM Saved | Frame Time Saved | Complexity |
|---|--------|-----------|-----------------|------------|
| 1a | memset in PixelMatrix::clear | 0 | ~0.1 ms | Trivial |
| 1b | Eliminate Color() pack-unpack | 0 | ~0.5 ms | Trivial |
| 1c | Cache numPixels() | 0 | ~0.2 ms | Trivial |
| 1d | Skip effect call when NONE | 0 | ~0.1 ms | Trivial |
| 1e | Right-size TFLite arena | **28 KB** | 0 | Trivial |
| 2a | LEDMapper uint16/uint8 arrays | **8 KB** | 0 | Low |
| 2b | EffectRenderer direct buffer write | 0 | ~1-2 ms | Medium |
| 2c | Pre-apply brightness | 0 | ~0.5 ms | Low |
| 3a | Custom Nrf52PwmLedStrip (async) | **48 KB saved** (no per-frame malloc) | **~10-13 ms** (async overlap) | Medium |
| 3b | I2S driver (alternative to 3a) | **36 KB more** vs PWM | same as 3a | Medium-High |

**After Phase 1+2:** ~36 KB RAM freed, ~2.5 ms CPU saved per frame. Still blocking at 22 FPS.
**After Phase 3a:** async DMA, 32 FPS, no per-frame malloc. This is the real unlock for 1024 LEDs on nRF52840.

Phase 1 items can be done together in one pass. Phase 2 items are independent. Phase 3 should be done last and tested on the bare test chip before flashing installed devices.
