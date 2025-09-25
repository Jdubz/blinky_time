# Visual Effects Architecture

## Overview

The blinky-things project has been refactored to separate visual effect generation from LED hardware rendering. This enables easier testing, effect swapping, and hardware abstraction.

## Architecture Components

### Core Classes

1. **`EffectMatrix`** - 2D RGB color buffer
   - Stores visual effect output as RGB values
   - Provides safe coordinate access and bounds checking
   - Includes debugging and testing utilities

2. **`VisualEffect`** - Base interface for all effects
   - `begin(width, height)` - Initialize effect with dimensions
   - `update(energy, hit)` - Update state based on audio input  
   - `render(matrix)` - Generate current frame into EffectMatrix
   - `restoreDefaults()` - Reset to default parameters

3. **`EffectRenderer`** - Renders EffectMatrix to physical LEDs
   - Handles LED mapping and hardware-specific color formats
   - Provides test patterns for hardware verification
   - Abstracts LED strip wiring patterns

4. **`FireVisualEffect`** - Fire simulation implementation
   - Refactored from original FireEffect
   - Uses heat diffusion simulation
   - Outputs to EffectMatrix instead of direct LED control

### Testing Infrastructure

5. **`FireEffectTest`** - Comprehensive test suite
   - Color palette validation
   - Heat-to-color conversion testing
   - Matrix generation verification
   - Audio responsiveness testing
   - Boundary condition testing

6. **`EffectTestRunner`** - Test execution framework
   - Serial console integration
   - Test result reporting
   - Quick validation modes

## Usage Example

```cpp
// Create effect and renderer
FireVisualEffect fireEffect;
EffectMatrix matrix(4, 15);
EffectRenderer renderer(leds, ledMapper);

// Initialize
fireEffect.begin(4, 15);

// Update loop
fireEffect.update(audioEnergy, audioHit);
fireEffect.render(matrix);
renderer.render(matrix);
renderer.show();
```

## Testing Commands

Available via serial console:

- `test all` - Run complete test suite
- `test fire` - Test fire effect specifically  
- `test quick` - Quick validation test
- `test colors` - Test color generation

## Benefits

1. **Testability** - Effects can be tested without hardware
2. **Modularity** - Easy to swap different effects
3. **Hardware Abstraction** - Same effect works on different LED configurations
4. **Maintainability** - Clear separation of concerns
5. **Debugging** - Matrix contents can be inspected and logged

## Migration Notes

- Original `FireEffect` remains functional for backward compatibility
- New `FireVisualEffect` provides identical visual output
- Serial console includes new test commands
- All existing functionality preserved

## Color Validation

The test suite verifies:
- Fire colors progress from black → dark red → bright red → orange → yellow → white
- Red dominance in fire colors (except hottest temperatures)
- Correct heat-to-color mapping
- Audio responsiveness affects color intensity
- Matrix coordinates map correctly to visual output

This ensures the fire effect displays proper red/orange fire colors rather than incorrect green colors.