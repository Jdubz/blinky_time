# Generator-Effect-Renderer Architecture

## 🎯 Overview

This document describes the reorganized visual architecture that separates concerns into three distinct stages:

1. **Generators** - Create visual patterns and output to EffectMatrix
2. **Effects** - Modify generated patterns (hue rotation, brightness, etc.)
3. **Renderers** - Map processed patterns to physical hardware

## 🏗️ Architecture Flow

```
Generator -> Effects -> Renderer -> Hardware
    ↓         ↓          ↓
  Pattern   Modify    Display
  Creation  Pattern   on LEDs
```

### Previous Architecture (Visual Effects)
```
FireVisualEffect -> LEDs (direct)
```

### New Architecture (Generator-Effect-Renderer)
```
FireGenerator -> HueRotationEffect -> EffectRenderer -> LEDs
```

## 📁 File Organization

### Directory Structure (current layout — 2026-05-19)

```text
blinky-things/
├── BlinkyArchitecture.h           # Main include for Arduino IDE
├── generators/                    # Pattern generators (flat — no per-generator subdirs)
│   ├── Generator.h                # Base interface
│   ├── Fire.{h,cpp}
│   ├── Water.{h,cpp}
│   ├── PlasmaGlobe.{h,cpp}
│   └── Audio.{h,cpp}
├── effects/                       # Visual effects
│   ├── Effect.h                   # Base interface
│   ├── HueRotationEffect.{h,cpp}
│   ├── NoOpEffect.h
│   └── tests/
├── render/                        # Rendering pipeline (was renderers/)
│   ├── EffectRenderer.{h,cpp}
│   ├── LEDMapper.h
│   ├── RenderPipeline.{h,cpp}
│   └── tests/
└── tests/                         # Cross-cutting tests
```

The earlier per-generator subdirectory layout (`generators/fire/FireGenerator.{h,cpp}`) was flattened; generator class names dropped the `Generator` suffix (`FireGenerator` → `Fire`). The conceptual three-stage pipeline (Generator → Effect → Renderer) below is unchanged.

## 🔧 Base Interfaces

### Generator Interface
```cpp
class Generator {
public:
    virtual void begin(int width, int height) = 0;
    virtual void generate(EffectMatrix* matrix) = 0;
    virtual void update() = 0;
    virtual const char* getName() const = 0;
};
```

### Effect Interface
```cpp
class Effect {
public:
    virtual void begin(int width, int height) = 0;
    virtual void apply(EffectMatrix* matrix) = 0;
    virtual const char* getName() const = 0;
};
```

## 🔥 Fire Generator

### Key Changes from FireVisualEffect
- **Interface**: Implements `Generator` instead of `VisualEffect`
- **Method**: `generate(EffectMatrix*)` instead of `render(EffectMatrix&)`
- **Audio Input**: `setAudioInput(energy, hit)` called before `update()`
- **Location**: `generators/fire/` directory with dedicated test suite

### Usage Pattern
```cpp
FireGenerator fireGen;
HueRotationEffect hueEffect(0.1f);  // 10% hue shift
EffectRenderer renderer;

// Setup
fireGen.begin(width, height);
hueEffect.begin(width, height);
renderer.begin(width, height, ledStrip);

// Each frame
fireGen.setAudioInput(energy, hit);
fireGen.update();
fireGen.generate(&matrix);
hueEffect.apply(&matrix);
renderer.render(&matrix);
```

## 🎨 Effects System

### HueRotationEffect
- **Purpose**: Shifts the hue of all colors in the matrix
- **Use Cases**: Blue fire, green fire, rainbow cycling
- **Parameters**: 
  - `hueShift` (0.0-1.0): Static hue shift amount
  - `rotationSpeed` (cycles/second): Auto-rotation speed

### Planned Effects
- **BrightnessEffect**: Overall brightness control
- **BlurEffect**: Spatial blur/smoothing
- **ColorMappingEffect**: Custom color palette mapping
- **PulseEffect**: Rhythmic brightness pulsing

## 🧪 Testing Architecture

### Generator-Specific Testing
Each generator type has its own comprehensive test suite:

- **FireGeneratorTest**: Tests fire-specific behavior (heat simulation, color generation)
- **FireTestRunner**: Command interface for fire tests
- **GeneratorTestRunner**: Coordinates all generator tests

### Test Commands
```
generators       - Run all generator tests
gen fire         - Run fire generator tests
fire all         - Run all fire-specific tests
fire color       - Test fire color generation
fire audio       - Test audio response
gen help         - Show all commands
```

## 🎯 Benefits of New Architecture

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

### Composability
```cpp
fireGen -> hueRotation -> brightness -> renderer  // Blue dimmed fire
starGen -> hueRotation -> pulse -> renderer       // Pulsing colored stars
waveGen -> blur -> brightness -> renderer         // Soft flowing waves
```

## 🚀 Migration Guide

### From Old FireVisualEffect
1. Replace `#include "FireVisualEffect.h"` with `#include "generators/fire/FireGenerator.h"`
2. Change `FireVisualEffect` to `FireGenerator`
3. Replace `update(energy, hit)` with:
   ```cpp
   setAudioInput(energy, hit);
   update();
   ```
4. Replace `render(matrix)` with `generate(&matrix)`

### Testing Migration
1. Replace `#include "FireEffectTest.h"` with `#include "generators/fire/FireGeneratorTest.h"`
2. Replace `EffectTestRunner` with `GeneratorTestRunner`
3. Update test commands from `fire test` to `gen fire` or `fire all`

## 📈 Future Architecture Extensions

### Planned Generators
- **StarGenerator**: Twinkling star field patterns
- **WaveGenerator**: Flowing wave animations
- **NoiseGenerator**: Perlin noise-based patterns
- **PlasmaGenerator**: Plasma/energy field effects

### Planned Effects
- **MotionBlurEffect**: Trailing/motion blur
- **CompositeEffect**: Blend multiple patterns
- **AudioReactiveEffect**: Real-time audio visualization
- **ParticleEffect**: Particle system overlay

### Hardware Renderers
- **MatrixRenderer**: 2D LED matrix mapping
- **StripRenderer**: 1D LED strip mapping  
- **SegmentRenderer**: Multi-segment hardware
- **NetworkRenderer**: Network-distributed LEDs

This architecture provides a solid foundation for complex visual effects while maintaining clear separation of concerns and high testability.