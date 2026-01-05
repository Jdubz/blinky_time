# Visualization Safety Testing

## CRITICAL: Why This Testing Exists

**Runaway brightness can physically melt LED controllers.**

### The Risk

LED controllers have thermal and current limits. Exceeding these limits causes:
- **Immediate risk**: Controller overheating and failure
- **Sustained risk**: Thermal damage, melted solder joints, fire hazard
- **Cascading failure**: Damaged controller can damage nRF52840 via backfeed

### Real-World Example: The Missing `clear()` Bug

**Commit 7999748** fixed a critical bug where `RenderPipeline.render()` never cleared the matrix between frames:

```cpp
// BEFORE (DANGEROUS):
void RenderPipeline::render(const AudioControl& audio) {
    currentGenerator_->generate(*pixelMatrix_, audio);  // Adds to previous frame
    renderer_->render(*pixelMatrix_);
}

// AFTER (SAFE):
void RenderPipeline::render(const AudioControl& audio) {
    pixelMatrix_->clear();  // ← CRITICAL: Prevents accumulation
    currentGenerator_->generate(*pixelMatrix_, audio);
    renderer_->render(*pixelMatrix_);
}
```

**Impact of this bug:**
- Frame 1: Fire generator renders, avg brightness = 50
- Frame 2: Fire renders again, **adds to Frame 1**, avg brightness = 100
- Frame 3: **adds to Frame 2**, avg brightness = 150
- Frame 10: **All LEDs saturated at RGB(255,255,255)** = 3x normal current draw
- Result: **LED controller thermal shutdown or permanent damage**

## Safety Test Suite

### RenderPipelineSafetyTest.h

Location: `blinky-things/tests/RenderPipelineSafetyTest.h`

#### Test 1: Frame Clearing Verification

**Prevents**: Missing `clear()` calls that cause brightness accumulation

**What it tests:**
- Creates test matrix
- Fills with known pattern
- Calls `clear()`
- Verifies all pixels are zero

**Failure indicates**: Rendering pipeline not clearing between frames

#### Test 2: Brightness Bounds Validation

**Prevents**: Pixel values exceeding RGB(255,255,255)

**What it tests:**
- Scans entire matrix after render
- Checks each channel: `r <= 255`, `g <= 255`, `b <= 255`
- Checks total brightness: `r + g + b <= 765`

**Failure indicates**: Overflow in blending operations (ADDITIVE, MAX)

#### Test 3: Color Accumulation Detection

**Prevents**: Gradual brightness increase over time

**What it tests:**
- Renders 10 frames with silent audio (no input)
- Measures brightness before and after
- Fails if brightness increases >50%

**Failure indicates**: Frame-to-frame accumulation (missing clear or improper blending)

#### Test 4: Thermal Protection

**Prevents**: Sustained max brightness causing thermal damage

**What it tests:**
- Counts pixels at max brightness RGB(255,255,255)
- Fails if >50% of LEDs at max for >100 consecutive frames (~3 seconds)

**Failure indicates**: Generator producing sustained max output

#### Test 5: Generator Output Validation

**Prevents**: Invalid/corrupted pixel data

**What it tests:**
- Scans matrix for invalid RGB values
- Checks for NaN, negative, or out-of-range values

**Failure indicates**: Generator producing corrupt data

## Running Tests

### At Development Time

Include in firmware for startup validation:

```cpp
#include "tests/RenderPipelineSafetyTest.h"

void setup() {
    Serial.begin(115200);

    // Initialize render pipeline
    RenderPipeline pipeline;
    pipeline.begin(config, leds, mapper);

    // Run safety tests BEFORE normal operation
    int failures = RenderPipelineSafetyTest::runAllTests(
        pipeline,
        *pipeline.getPixelMatrix(),
        true  // verbose
    );

    if (failures > 0) {
        Serial.println(F("SAFETY TEST FAILURES - HALTING"));
        while(1) { delay(10000); }  // HALT - do not proceed
    }
}
```

### In CI/CD Pipeline

Add to GitHub Actions or build scripts:

```bash
# Compile firmware with tests enabled
arduino-cli compile --fqbn Seeeduino:mbed:xiaonRF52840Sense blinky-things

# Run unit tests (native x86 build)
cd tests
g++ -DTEST_MODE -I.. RenderPipelineTests.cpp -o test_render
./test_render

# Parse output for failures
if grep "FAIL" test_output.txt; then
    echo "SAFETY TESTS FAILED - BLOCKING MERGE"
    exit 1
fi
```

### Continuous Monitoring

Use `BrightnessMonitor` class for runtime protection:

```cpp
#include "tests/RenderPipelineSafetyTest.h"

RenderPipelineSafetyTest::BrightnessMonitor monitor;

void loop() {
    // Render frame
    renderPipeline.render(audioControl);

    // Check for thermal issues
    monitor.checkFrame(*renderPipeline.getPixelMatrix());

    // If emergency detected, monitor.checkFrame() halts system
}
```

## Test Coverage Requirements

All code touching the rendering pipeline MUST have tests:

| Component | Required Tests | Critical? |
|-----------|---------------|-----------|
| Generator::generate() | Output bounds, accumulation | YES |
| Effect::apply() | No overflow, bounds preserved | YES |
| RenderPipeline::render() | Frame clearing, no accumulation | YES |
| PixelMatrix::setPixel() | Bounds checking, saturation | YES |
| Blending operations | No overflow (ADDITIVE, MAX) | YES |

## When to Run Tests

### MANDATORY Test Points

1. **Before every firmware upload** - Run full test suite
2. **After any rendering code change** - Run affected tests
3. **In CI/CD pipeline** - Block merge on test failure
4. **At device startup** - Runtime validation

### WARNING Test Points

1. **After parameter changes** - Verify no overflow
2. **After palette changes** - Verify color bounds
3. **After timing changes** - Verify no accumulation

## What To Do When Tests Fail

### Failure: Frame Clearing

**Root cause**: Missing `pixelMatrix_->clear()` call

**Fix**:
```cpp
void render() {
    pixelMatrix_->clear();  // ← ADD THIS
    generator->generate(*pixelMatrix_, audio);
    renderer->render(*pixelMatrix_);
}
```

### Failure: Brightness Bounds

**Root cause**: Overflow in blending operations

**Fix**:
```cpp
// WRONG - can overflow:
matrix.setPixel(x, y, existing.r + newR, existing.g + newG, existing.b + newB);

// CORRECT - saturate at 255:
matrix.setPixel(x, y,
    min(255, existing.r + newR),
    min(255, existing.g + newG),
    min(255, existing.b + newB));
```

### Failure: Color Accumulation

**Root causes**:
1. Missing `clear()` before render
2. Generator not clearing internal state
3. Effect accumulating values

**Fix**: Add clear() calls at appropriate points

### Failure: Thermal Protection

**Root cause**: Generator producing sustained max output

**Fix**:
```cpp
// WRONG - sustained max brightness:
for (all pixels) {
    matrix.setPixel(x, y, 255, 255, 255);
}

// CORRECT - modulate brightness:
for (all pixels) {
    uint8_t brightness = audio.energy * 255;  // 0-255 based on input
    matrix.setPixel(x, y, brightness, brightness, brightness);
}
```

### Failure: Generator Output

**Root cause**: Corrupt data from generator

**Fix**: Add bounds checking to generator output

## Hardware Safety Margins

### Current Limits

Typical APA102/SK9822 LED:
- Per channel: 20mA @ 255 brightness
- Per LED (RGB): 60mA @ full white (255,255,255)
- String of 89 LEDs: 5.34A @ full white

nRF52840 limits:
- Total current budget: ~100mA
- External power required for LED strips

**Safety margins**:
- Keep avg brightness <50% to prevent thermal issues
- Use current-limiting resistors on data lines
- Monitor temperature if possible

### Thermal Limits

LED controller thermal limits vary, but generally:
- Continuous operation: <80°C
- Emergency shutdown: >100°C
- Permanent damage: >125°C

**Protection strategy**:
- Limit sustained max brightness to 3 seconds
- Modulate brightness based on audio (natural variation)
- Implement thermal monitoring if temperature sensors available

## Future Enhancements

Potential additions to safety testing:

1. **Current estimation** - Calculate total current draw, warn if exceeds safe limits
2. **Temperature monitoring** - Integrate thermal sensors, emergency shutdown on overheat
3. **Brightness histograms** - Track brightness distribution over time
4. **Power consumption logging** - Record sustained high-power events
5. **Automated regression tests** - Run full test suite on every commit

## References

- **Missing clear() bug**: Commit 7999748
- **Heat accumulation bug**: Commit 1d583ab
- **Lightning underflow bug**: Commit f28afb3
- **Safety test framework**: `blinky-things/tests/SafetyTest.h`
- **Static initialization safety**: `blinky-things/tests/StaticInitCheck.h`
