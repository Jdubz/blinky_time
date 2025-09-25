# Blinky Things - Folder Structure

This document describes the organized folder structure for the Generator-Effect-Renderer architecture.

## 📁 Directory Structure

```
blinky-things/
├── BlinkyArchitecture.h           # Main include file for Arduino IDE
├── Generator.h                    # Base generator interface
├── Effect.h                       # Base effect interface  
├── EffectMatrix.h/cpp             # Visual buffer/matrix
├── SerialConsole.h/cpp            # Serial command interface
├── configs/                       # Device configurations
├── generators/                    # Pattern generators
│   └── fire/                      # Fire generator
│       ├── FireGenerator.h/cpp    # Fire pattern implementation
│       └── tests/                 # Fire generator tests
│           ├── FireGeneratorTest.h/cpp
│           └── FireTestRunner.h/cpp
├── effects/                       # Visual effects/modifiers
│   └── hue-rotation/              # Hue rotation effect
│       ├── HueRotationEffect.h/cpp
│       └── tests/                 # Hue rotation tests
│           └── HueRotationEffectTest.h/cpp
├── renderers/                     # Hardware renderers
│   ├── EffectRenderer.h/cpp       # LED strip/matrix renderer
│   └── tests/                     # Renderer tests (future)
└── tests/                         # Main test coordination
    └── GeneratorTestRunner.h/cpp  # Overall test runner
```

## 🎯 Design Principles

### 1. **Co-location of Tests**
- Tests live next to the code they test
- Each component has its own `tests/` subdirectory
- Easy to find and maintain component-specific tests

### 2. **Clear Component Separation**
- **`generators/`** - Pattern creation (fire, stars, waves, etc.)
- **`effects/`** - Pattern modification (hue rotation, brightness, blur, etc.)
- **`renderers/`** - Hardware output (LED strips, matrices, etc.)

### 3. **Hierarchical Organization**
- Each major component type gets its own directory
- Individual implementations get subdirectories
- Tests are co-located with their implementations

### 4. **Arduino IDE Compatibility**
- `BlinkyArchitecture.h` provides single include for main sketch
- Relative includes work properly with subfolder structure
- Main sketch stays clean and focused

## 🔧 Usage Patterns

### For Arduino IDE Main Sketch
```cpp
#include "BlinkyArchitecture.h"

FireGenerator fireGen;
HueRotationEffect hueEffect;
EffectRenderer renderer;
EffectMatrix matrix(4, 15);

void setup() {
  fireGen.begin(4, 15);
  hueEffect.begin(4, 15);
  renderer.begin(4, 15, &leds);
}

void loop() {
  fireGen.update();
  fireGen.generate(&matrix);
  hueEffect.apply(&matrix);
  renderer.render(&matrix);
}
```

### For Development/Testing
```cpp
#define ENABLE_TESTING
#include "BlinkyArchitecture.h"

// Now you have access to GeneratorTestRunner
GeneratorTestRunner testRunner;
```

### For Adding New Generators
1. Create `generators/myGenerator/` directory
2. Add `MyGenerator.h/cpp` files  
3. Create `generators/myGenerator/tests/` directory
4. Add test files and runner
5. Update `BlinkyArchitecture.h` to include new generator

### For Adding New Effects
1. Create `effects/my-effect/` directory
2. Add `MyEffect.h/cpp` files
3. Create `effects/my-effect/tests/` directory  
4. Add test files
5. Update `BlinkyArchitecture.h` to include new effect

## 🧪 Testing Architecture

### Component-Level Testing
Each component has its own focused test suite:
- **FireGeneratorTest** - Fire pattern accuracy, heat simulation
- **HueRotationEffectTest** - Color transformation correctness
- **EffectRendererTest** - Hardware mapping accuracy (future)

### Integration Testing
Main test runner coordinates all component tests:
- **GeneratorTestRunner** - Runs tests for all generators and effects
- Provides unified command interface for serial console
- Enables comprehensive system validation

### Test Commands
All tests accessible through serial console:
```
generators              # Run all tests
gen fire               # Fire generator tests
effect hue             # Hue rotation tests (future)
test help              # Show all commands
```

## 🚀 Benefits

### Development Benefits
- **Clear Structure** - Easy to navigate and understand
- **Focused Testing** - Each component thoroughly tested
- **Easy Extension** - Adding new generators/effects is straightforward
- **Maintainable** - Related code stays together

### Arduino IDE Benefits  
- **Single Include** - Just `#include "BlinkyArchitecture.h"`
- **Clean Main Sketch** - Focus on application logic
- **Proper Dependencies** - All includes resolved automatically

### Production Benefits
- **Modular Architecture** - Components can be swapped easily
- **Comprehensive Testing** - High confidence in functionality
- **Extensible Design** - Easy to add new visual effects
- **Professional Structure** - Enterprise-grade organization

This structure scales from simple single-effect projects to complex multi-layer visual systems while maintaining clarity and testability.