# LED Render Optimization Plan

*Created: March 29, 2026*

Target hardware: ESP32-S3, 240MHz, FreeRTOS. 32×32 = 1024 WS2812B LEDs.
Current state: ~20fps, irregular frame timing. WiFi radio preemption fixed (disabled when unconfigured). Redundant EffectRenderer pre-clear removed.

---

## Findings

### Memory — two full pixel copies in RAM

| Buffer | Type | Size |
|--------|------|------|
| PixelMatrix | `RGB[1024]`, heap | 3,072 bytes |
| Adafruit_NeoPixel internal | `uint8_t[1024*3]`, heap | 3,072 bytes |
| LEDMapper `positionToIndex` | `int[1024]` | 4,096 bytes |
| LEDMapper `indexToX` | `int[1024]` | 4,096 bytes |
| LEDMapper `indexToY` | `int[1024]` | 4,096 bytes |
| **Total** | | **~18.4 KB** |

PixelMatrix (RGB order) and the Adafruit buffer (GRB order) are two independent heap allocations holding the same frame simultaneously. LEDMapper wastes 8 KB using `int` for values that fit in `uint16_t`/`uint8_t`.

### Per-frame CPU waste

**Adafruit espShow() reinstalls the RMT driver every frame.**
`espShow()` calls `rmt_config()`, `rmt_driver_install()`, `rmt_translator_init()`, and `rmt_driver_uninstall()` on every call. These were designed for one-shot use. The wire time for 1024 LEDs is unavoidably ~31ms, but driver init/deinit adds 1-2ms on top, and — more importantly — `rmt_write_sample(..., true)` blocks the CPU for the entire 31ms with no overlap with generation.

**Pack-unpack round trip per pixel (1024 times per frame).**
`EffectRenderer::render()` calls `leds_.Color(r,g,b)` → packs to `uint32_t` → `setPixelColor(index, uint32)` → immediately unpacks back to r,g,b → applies brightness multiply → reorders to GRB. The pack step is pure waste.

**Per-pixel brightness multiply even though brightness is constant.**
`setPixelColor()` applies brightness as 3 multiplies + 3 shifts per pixel on every call, even though `setBrightness()` is called once at startup and never changes. 3,072 multiplies per frame for a constant.

**Effect system called unconditionally even when no effect is active.**
`currentEffect_->apply(pixelMatrix_)` dispatches through a virtual call to `NoOpEffect::apply()` which does nothing. The call chain + null suppression still runs every frame.

**`PixelMatrix::clear()` uses a struct-assign loop instead of memset.**
`pixels_[i] = RGB(0,0,0)` for 1024 pixels. The compiler likely optimizes this, but `memset` is explicit and guaranteed fast. `RGB(0,0,0)` is an all-zero byte pattern.

---

## Plan

### Phase 1 — Trivial wins (no architectural change)

**1a. `memset` in `PixelMatrix::clear()`**
Replace the `RGB(0,0,0)` loop with `memset(pixels_, 0, width_ * height_ * sizeof(RGB))`.
- Impact: negligible speed, architecturally correct.
- Risk: none. One line.

**1b. Eliminate Color()/setPixelColor pack-unpack**
`EffectRenderer::render()`: replace `leds_.setPixelColor(idx, leds_.Color(r,g,b))` with the three-argument form `leds_.setPixelColor(idx, r, g, b)`. Also fix `NeoPixelLedStrip::setPixelColor(index, r, g, b)` to call `strip_->setPixelColor(index, r, g, b)` directly instead of re-packing with `strip_->Color()`.
- Impact: eliminates 1,024 Color() calls + 2,048 virtual dispatches per frame. ~0.5ms.
- Risk: none. Two files, ~5 lines.

**1c. Cache `numPixels()` outside the render loop**
`leds_.numPixels()` is a virtual call made once per pixel iteration. Call it once before the loop.
- Impact: eliminates 1,024 virtual dispatches per frame. ~0.2ms.
- Risk: none. One line.

**1d. Skip effect call entirely when no effect is active**
In `RenderPipeline::render()`, guard the `currentEffect_->apply()` call:
```cpp
if (effectType_ != EffectType::NONE) {
    currentEffect_->apply(pixelMatrix_);
}
```
The `noOp_` instance and `NoOpEffect` class can also be removed entirely — `currentEffect_` set to `nullptr` when `effectType_ == NONE`, guarded by the check above. Saves a heap allocation and a class definition.
- Impact: eliminates virtual dispatch + function call on every frame when no effect active (the common case). Simplifies the effect model.
- Risk: none. ~10 lines.

**1e. Skip no-op operations in other pipeline stages**
Audit and guard other operations that run unconditionally but may have nothing to do:
- **Heat grid in Fire**: `updateHeatGrid()` runs every frame. If both `buoyancyCoupling == 0` and `pressureCoupling == 0`, skip grid update entirely. `applyGridForce()` already guards, but `updateHeatGrid()` (including the cool loop and splat) still runs.
- **ForceAdapter wind update**: `forceAdapter_->update(dt)` updates curl noise every frame. If `windVariation == 0`, the result is unused — the force adapter should short-circuit when wind variance is zero.
- **HueRotation**: already skips black pixels. Good.
- Impact: small individually, but eliminates classes of wasted work when features are at default/disabled values.
- Risk: low. Each is a simple guard.

---

### Phase 2 — Architecture (medium effort)

**2a. Shrink LEDMapper arrays — saves 8 KB**
`positionToIndex`: `int[N]` → `uint16_t[N]` (all LED indices fit in uint16, max 65535)
`indexToX`, `indexToY`: `int[N]` → `uint8_t[N]` (max dimension 255, sufficient for any device)
Use `0xFFFF` as the invalid sentinel for `positionToIndex` instead of `-1`.
- Impact: **8 KB RAM saved** (12 KB → 4 KB) for a 1024-LED device.
- Risk: low. Requires updating sentinel checks. ~30 lines across LEDMapper.h/.cpp.

**2b. EffectRenderer: iterate by LED index, write direct to Adafruit buffer**
Currently iterates (x,y) → `positionToIndex[y*w+x]` lookup → setPixelColor. Change to iterate `ledIndex` 0..N-1, read `matrix.getPixel(indexToX[i], indexToY[i])`, write GRB bytes directly into the Adafruit `getPixels()` buffer with inline brightness and GRB reordering. Writes to the output buffer become sequential (cache-friendly). Eliminates all virtual dispatch in the inner loop.
- Impact: eliminates `positionToIndex` lookup per pixel, eliminates all virtual dispatch in hot path, sequential writes improve cache behavior. ~2-3ms saved.
- Risk: low-medium. Requires `getPixels()` access from `NeoPixelLedStrip`. ~40 lines.

**2c. Pre-apply brightness once, disable per-pixel multiply**
Call `setBrightness(255)` (which Adafruit internally stores as 0 = "no scaling") during `begin()`. Apply the actual brightness value once during the EffectRenderer copy in 2b rather than letting Adafruit do it per-pixel. Eliminates 3,072 multiplies+shifts per frame.
- Impact: ~0.5ms saved per frame.
- Risk: low, requires understanding Adafruit's brightness encoding. ~20 lines.

---

### Phase 3 — Async RMT (high impact)

**3a. New `Esp32RmtLedStrip` with persistent IDF 5 RMT + DMA**

The ESP32-S3 with arduino-esp32 3.x (IDF 5.x) has a new RMT driver (`esp_driver_rmt.h`) that supports:
- Persistent channel handles (no reinstall per frame)
- DMA-backed transmission (RMT hardware handles bit timing autonomously)
- Non-blocking `rmt_transmit()` that returns in <100µs

Implementation:
- Create `blinky-things/hal/hardware/Esp32RmtLedStrip.h/.cpp` implementing `ILedStrip`
- `begin()`: `rmt_new_tx_channel()` with `flags.with_dma = true`, `rmt_new_bytes_encoder()` with WS2812B timings (T0H=400ns, T0L=850ns, T1H=800ns, T1L=450ns), `rmt_enable()`
- `show()`: call `rmt_tx_wait_all_done(channel, 0)` to ensure previous frame finished, then `rmt_transmit(channel, encoder, pixels, len, &config)` — returns immediately
- Double-buffer: two GRB byte arrays (`buf_[2]`), swap index each frame. Generation writes into buf_[write], show() transmits buf_[read]. Prevents tearing when DMA reads the buffer asynchronously.
- Conditional compile: `#ifdef BLINKY_PLATFORM_ESP32S3`

**Result:** CPU and LED transmission overlap completely. With 31ms wire time and ~5-8ms generation time, the CPU is idle for ~23ms per frame — yielding the BLE task for free. Theoretical frame rate rises to generation-limited (~125fps), practical ~50-60fps smooth.

WS2812B encoder timing (IDF 5 bytes encoder config):
```c
rmt_bytes_encoder_config_t enc_cfg = {
    .bit0 = { .duration0 = 4, .level0 = 1, .duration1 = 8, .level1 = 0 }, // 400ns/800ns @10MHz
    .bit1 = { .duration0 = 8, .level0 = 1, .duration1 = 4, .level1 = 0 }, // 800ns/400ns
    .flags.msb_first = 1,
};
```

- Impact: **~30fps gain** (20fps → 50+fps), frame timing becomes highly consistent.
- Risk: medium. New API, double-buffer complexity, ESP32-S3 only. ~150-200 lines. Test on display device before deploying to blinkyhost.
- Files: new `Esp32RmtLedStrip.h/.cpp`, update `blinky-things.ino` conditional init.

---

## Implementation Order

| # | Change | RAM Saved | Frame Time Saved | Complexity |
|---|--------|-----------|-----------------|------------|
| 1a | memset in PixelMatrix::clear | 0 | ~0.1ms | Trivial |
| 1b | Eliminate Color() pack-unpack | 0 | ~0.5ms | Trivial |
| 1c | Cache numPixels() | 0 | ~0.2ms | Trivial |
| 1d | Skip effect call when NONE | 0 | ~0.2ms | Trivial |
| 1e | Guard heat grid, wind adapter no-ops | 0 | ~0.2ms | Trivial |
| 2a | LEDMapper uint16/uint8 arrays | **8 KB** | 0 | Low |
| 2b | EffectRenderer direct buffer write | 0 | ~2-3ms | Medium |
| 2c | Pre-apply brightness | 0 | ~0.5ms | Medium |
| 3a | Async RMT + DMA (Esp32RmtLedStrip) | 0 | **~15-25ms** | Medium-High |

Phase 1 items can be done together in one pass. Phase 2 items are independent. Phase 3 should be done last and tested on the unenclosed test chip before flashing installed devices.
