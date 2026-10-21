# Generator-Effect-Renderer Architecture

## Overview

The visual side of the firmware has three stages:

1. **Generators** - Create visual patterns into a `PixelMatrix`, driven by `AudioControl`
2. **Effects** - Optionally modify the generated pattern (hue rotation)
3. **Renderer** - Maps the matrix to the physical LEDs (`EffectRenderer` + `LEDMapper`)

`RenderPipeline` (`render/RenderPipeline.{h,cpp}`) owns all of them. It keeps exactly one active generator and zero or one active effect.

## Architecture Flow

```
AudioTracker -> AudioControl -> Generator -> Effect (optional) -> FrameMetrics -> EffectRenderer -> LEDs
                                    ↓              ↓                   ↓               ↓
                                 Pattern        Modify            Measure          Display
                                 Creation       Pattern            frame           on LEDs
```

Each frame, the main loop (`blinky-things.ino`) updates the audio tracker, then calls `pipeline->render(audio)` and `leds->show()`. `RenderPipeline::render()`:

```cpp
pixelMatrix_->clear();
currentGenerator_->generate(*pixelMatrix_, audio);
if (effectType_ != EffectType::NONE) currentEffect_->apply(pixelMatrix_);
metrics_.processFrame(*pixelMatrix_);   // brightness/contrast/activity (`show visual`)
renderer_->render(*pixelMatrix_);
```

## File Organization

### Directory Structure

```text
blinky-things/
├── BlinkyArchitecture.h           # Main include for Arduino IDE
├── generators/                    # Pattern generators (flat — no per-generator subdirs)
│   ├── Generator.h                # Base interface + GeneratorType
│   ├── Fire.{h,cpp}               # Particle generator: heightmap flame + bursts
│   ├── Water.{h,cpp}              # Particle generator: drops, splashes, ripples
│   ├── PlasmaGlobe.{h,cpp}        # Drifting orbs (replaced Lightning in v94)
│   └── Audio.{h,cpp}              # Audio-signal visualizer (debug)
├── particles/                     # ParticleGenerator base, Particle, ParticlePool
├── physics/                       # Spawn regions, boundaries, forces, backgrounds
├── effects/                       # Visual effects
│   ├── Effect.h                   # Base interface
│   ├── HueRotationEffect.{h,cpp}
│   ├── NoOpEffect.h
│   └── tests/
├── render/                        # Rendering pipeline
│   ├── EffectRenderer.{h,cpp}
│   ├── FrameMetrics.h
│   ├── LEDMapper.h
│   ├── RenderPipeline.{h,cpp}
│   └── tests/
├── types/FrameClock.h             # Frame timing (absolute time, #259)
└── tests/                         # On-device test helpers (ENABLE_TESTING builds)
```

Generator class names have no `Generator` suffix (`Fire`, not `FireGenerator`). `Fire` and `Water` derive from `ParticleGenerator` (`particles/ParticleGenerator.h`), which derives from `Generator`.

### Generators and serial names

| `GeneratorType` | Class | `gen` / `scene` name | Settings category |
|-----------------|-------|----------------------|-------------------|
| `FIRE` | `Fire` | `fire` | `fire` |
| `WATER` | `Water` | `water` | `water` |
| `LIGHTNING` | `PlasmaGlobe` | `lightning` or `plasma` (`gen list` shows `plasma`) | `plasma` |
| `AUDIO` | `Audio` | `audio` | `audiovis` |

`scene <generator> <off|rotate|static> <speed 0-2> <hue 0-1>` sets generator and effect in one command (used for fleet broadcasts). A device with `buttonPin` set cycles generators on a button press.

## Base Interfaces

### Generator Interface (`generators/Generator.h`)

```cpp
class Generator {
public:
    virtual ~Generator() = default;
    virtual bool begin(const DeviceConfig& config) = 0;
    virtual void generate(PixelMatrix& matrix, const AudioControl& audio) = 0;
    virtual void reset() = 0;
    virtual const char* getName() const = 0;
    virtual GeneratorType getType() const = 0;

protected:
    uint16_t width_, height_, numLeds_;
    LayoutType layout_;
    MatrixOrientation orientation_;
    FrameClock frameClock_;          // real seconds per frame, never clamped (#259)
    float traversalDim_, crossDim_;  // set by computeDimensionScales() in begin()
};
```

Generators read only the `AudioControl` struct (`audio/AudioControl.h`: `energy`, `level`, `pulse`, `onset`, `phase`, `beatCount`, `plpPulse`, `rhythmStrength`, `onsetDensity`). They don't know about the mic, FFT or tracker internals. Physics parameters scale with `traversalDim_` and `crossDim_`, so one parameter set works on any LED layout.

### Effect Interface (`effects/Effect.h`)

```cpp
class Effect {
public:
    virtual ~Effect() = default;
    virtual void begin(int width, int height) = 0;
    virtual void apply(PixelMatrix* matrix) = 0;
    virtual void reset() = 0;
    virtual const char* getName() const = 0;
};
```

### Timing: animations keep absolute time (#259)

A late frame must land where it belongs in time, not stretch the animation. A generator gets its frame time from `frameClock_.tick(millis())` (`types/FrameClock.h`). That's the real elapsed time, never clamped, and 0 on the first tick after construction or `frameClock_.restart()`.
- **Clocks and decays** take the whole frame: noise time, drift, hue, palette bias, and envelopes written in closed form (`expf(-dt / tau)`, `powf(rate, dt * 60)`).
- **Explicit physics** (forces, velocities, heat diffusion, orb drift, ripples) runs the same time in bounded substeps, `FrameTiming::physicsSteps(dt)`: at most 50 ms each, over at most the last 1 s of a long gap (particles live ≤ 2.55 s).
- **Spawning** is edge-triggered on onsets and beats, so it runs once per frame over `FrameTiming::physicsSpan(dt)`.
- Never add a `dt` clamp or a "gap too long, assume 16 ms" fallback. `tests/timing/timing_test.cpp` checks that one 2 s frame ends with the same time-driven state as two hundred 10 ms frames.

The audio side follows the same rule: the rhythm `phase` and `plpPulse` are extrapolated to the moment of drawing (see `docs/AUDIO_ARCHITECTURE.md`, "Frame Clock").

## Fire Generator

`Fire` is a particle generator with a heightmap flame:
- A noise-shaped "smolder" flame sits at `smolderheight` and rises with the audio, up to `maxflameheight`.
- Bursts of fast, short-lived sparks fire on each onset event (`audio.onset > 0`) whose strength reaches `burstthreshold`, and on each new `beatCount` when the rhythm is locked (`rhythmStrength > 0.5`). Burst size grows with strength.
- A heat grid (`gridcoolrate`, `buoyancycoupling`, `pressurecoupling`) adds plume buoyancy and clustering.
- It reads `energy`, `level`, `pulse`, `onset`, `beatCount`, `plpPulse` and `rhythmStrength`.

All parameters are serial settings in the `fire` category (see `docs/AUDIO-TUNING-GUIDE.md`).

### Usage Pattern

You don't drive generators by hand; `RenderPipeline` does:

```cpp
RenderPipeline pipeline;
pipeline.begin(config, leds, mapper);

pipeline.setGenerator(GeneratorType::FIRE);
pipeline.setEffect(EffectType::HUE_ROTATION);   // or EffectType::NONE

// Each frame
const AudioControl& audio = tracker.update(dt);
pipeline.render(audio);
leds.show();
```

## Effects System

### HueRotationEffect
- **Purpose**: Shifts the hue of all colors in the matrix
- **Use Cases**: Blue fire, green fire, rainbow cycling
- **Serial**: `effect hue` / `effect none`; settings `hueshift` (0.0-1.0, static shift) and `huespeed` (cycles/second)

### Planned Effects (not built)
- **BrightnessEffect**: Overall brightness control
- **BlurEffect**: Spatial blur/smoothing
- **ColorMappingEffect**: Custom color palette mapping
- **PulseEffect**: Rhythmic brightness pulsing

## Testing Architecture

- **Host (CI):** `tests/timing/timing_test.cpp` runs every generator under UBSan and checks absolute timing (the "Absolute Timing" job in `.github/workflows/pr-validation.yml`).
- **Host (not in CI):** `tests/unit/` has older audio, fire and LED-mapper tests.
- **On device, `ENABLE_TESTING` builds only:** `tests/GeneratorTestRunner`, `effects/tests/` and `render/tests/`. These aren't wired to serial commands; the old `generators` / `fire all` / `gen help` test commands no longer exist (`gen` now switches generators).
- **Render safety:** see `docs/VISUALIZATION_SAFETY_TESTING.md`.

## Benefits of This Architecture

### Modularity
- **Generators** focus only on pattern creation
- **Effects** are reusable across different generators
- **Renderers** handle hardware-specific concerns

### Testability
- Each component can be tested independently
- Generator tests focus on pattern accuracy
- Effect tests focus on transformation correctness
- Renderer tests focus on hardware mapping

### Extensibility
- New generators don't affect existing effects
- New effects work with all generators
- New renderers support different hardware

## Adding a Generator

1. Derive from `Generator` (or `ParticleGenerator` for particle systems) in `generators/`.
2. Use `frameClock_.tick(millis())` for time and `FrameTiming::physicsSteps()` for integration.
3. Add a `GeneratorType`, wire it into `RenderPipeline` (`begin`, `setGenerator`, the name/type tables, `NUM_GENERATORS`) and `SerialConsole::handleGeneratorCommand()` / `handleSceneCommand()`.
4. Add the `.cpp` to `BlinkyImplementations.h` (Arduino builds only compile listed `.cpp` files in subdirectories).
5. If its parameters should persist, add a `Stored*Params` struct to `ConfigStorage.h` and bump `SETTINGS_VERSION`.

## Future Architecture Extensions

### Generator ideas (not built)
- **StarGenerator**: Twinkling star field patterns
- **WaveGenerator**: Flowing wave animations
- **NoiseGenerator**: Perlin noise-based patterns

(The plasma idea shipped as `PlasmaGlobe`.)

### Effect ideas (not built)
- **MotionBlurEffect**: Trailing/motion blur
- **CompositeEffect**: Blend multiple patterns
- **ParticleEffect**: Particle system overlay

### Hardware Renderers
`EffectRenderer` + `LEDMapper` map the matrix to the LED strip for matrix and linear layouts; two-strand devices (`ledPin2`) are handled below them by `hal/hardware/CompositeLedStrip.h`. Ideas not built: segment and network-distributed renderers.
