# Fire Totem Project - Analysis Notes

## Project Overview
**Type:** Arduino-based interactive LED fire simulation
**Hardware:** Seeed XIAO BLE Sense nRF52840, 16x8 cylindrical LED matrix (128 pixels), LSM6DS3 IMU, PDM microphone
**Physical Scale:** Cylindrical LED matrix, designed for torch/totem effects
**Core Concept:** Realistic fire simulation with motion-responsive orientation and audio reactivity

## Current Project State (Post-Cleanup)
**Status:** Clean, functional codebase with simplified motion physics
**Key Changes:** Removed complex torch physics, streamlined IMU interface, maintained cylinder orientation visualization
**Working Features:** Fire simulation, audio reactivity, cylinder orientation detection, serial console debugging

## Architecture Overview

### Component Hierarchy
```
fire-totem.ino (main loop)
├── FireEffect (visual fire simulation engine)
├── AdaptiveMic (audio processing & analysis)
├── IMUHelper (simplified motion tracking & orientation)
├── SerialConsole (debugging & visualization modes)
├── BatteryMonitor (power management)
└── TotemDefaults (configuration constants)
```

### Data Flow Patterns
```
IMU Sensors → IMUHelper → Orientation Data → FireEffect → LED Matrix
Audio Input → AdaptiveMic → Energy/Transient → FireEffect
Serial Commands → SerialConsole → Parameter Updates + Visualization Modes
```

## Core Components Analysis

### FireEffect.h/cpp - Visual Fire Simulation Engine

**Key Responsibilities:**
- Heat map-based fire simulation (128 float values)
- Realistic color palette (black→red→orange→yellow→white)
- Turbulence and noise-based organic patterns
- Motion-responsive flame physics

**Critical Data Structures:**
```cpp
float* heat;          // Main heat buffer [WIDTH*HEIGHT]
float* heatScratch;   // Temporary buffer for wind advection
FireParams params;    // Tunable fire behavior parameters
```

**Physics Implementation:**
- **Heat Propagation:** 3-point upward blur with turbulent decay
- **Cooling:** Random per-cell cooling with height-based scaling
- **Spark Generation:** Probabilistic with audio and motion enhancement
- **Wind Effects:** Horizontal advection with wrap-around
- **Turbulence:** Multi-octave noise for organic flame shapes

**Motion Integration Points:**
```cpp
// Enhanced motion inputs
void setTorchMotion(windX, windY, stoke, turbulence, centrifugal, flameBend, tilt, intensity);
void setRotationalEffects(spin, centrifugal);
void setInertialDrift(driftX, driftY);
```

**Performance Characteristics:**
- **Memory:** ~2KB for heat buffers (WIDTH*HEIGHT*sizeof(float)*2)
- **CPU:** Dominant cost in nested loops (propagateUp, coolCells, injectSparks)
- **Frame Rate:** Target ~60Hz, actual depends on complexity

**Extension Points:**
- Additional noise functions for variety
- Color palette variations
- Alternative physics models
- Multi-layer heat simulation

---

### AdaptiveMic.h/cpp - Advanced Audio Analysis

**Key Responsibilities:**
- Real-time audio processing with frequency analysis
- Adaptive gain control for different environments
- Musical pattern detection and beat tracking
- Environment classification (quiet→extreme)

**Critical Data Structures:**
```cpp
// ISR-based audio collection
volatile uint64_t s_sumAbs, s_numSamples;
volatile uint16_t s_maxAbs;

// Frequency analysis (lightweight FFT alternative)
float freqBuffer[FREQ_BUFFER_SIZE];
float bassLevel, midLevel, highLevel, spectralCentroid;

// Environment adaptation
AudioEnvironment currentEnv;  // ENV_QUIET→ENV_EXTREME
float compRatio, compThresh;  // Dynamic range compression
```

**Audio Processing Pipeline:**
1. **ISR Collection:** PDM samples → running averages
2. **Envelope Following:** Attack/release smoothing
3. **Frequency Analysis:** 3-band spectral analysis (bass/mid/high)
4. **Environment Detection:** Classify audio environment
5. **Adaptive Processing:** Auto-adjust parameters per environment
6. **Transient Detection:** Beat/hit detection with spectral awareness

**Advanced Features:**
- **Spectral Analysis:** Time-domain approximation of frequency content
- **Beat Detection:** BPM estimation from pattern analysis
- **Environment Classification:** 6-level environment detection
- **Dynamic Range Compression:** Professional-grade compressor
- **Musical Adaptation:** Genre-aware parameter adjustment

**Tunable Parameters:**
```cpp
float attackSeconds, releaseSeconds;     // Envelope response
float agTarget, agStrength;              // Auto-gain control
float noiseGate;                         // Noise suppression
float transientFactor, loudFloor;       // Beat detection
```

**Performance Notes:**
- ISR runs at 16kHz sample rate
- Main processing at ~60Hz frame rate
- Memory efficient - no full FFT required
- Adaptive parameters prevent clipping/saturation

---

### IMUHelper.h/cpp - Simplified Motion Tracking & Orientation

**Key Responsibilities (Post-Cleanup):**
- Basic 6DOF sensor reading (accelerometer, gyroscope, temperature)
- Gravity estimation for orientation detection
- Cylinder "up" vector calculation for fire effect
- Clean IMU data interface for debugging/visualization

**Critical Data Structures:**
```cpp
struct IMUData {
    Vec3 accel, gyro;           // Raw sensor readings
    Vec3 gravity, linearAccel;  // Processed motion data
    Vec3 up;                    // Orientation vector
    float tiltAngle;            // Degrees from vertical
    float motionMagnitude;      // Overall motion level
    bool isMoving;              // Basic motion detection
    unsigned long timestamp;    // Data capture time
};

struct MotionState {
    Vec3 up;                    // Unit vector (world up in torch space)
    float tiltAngle;            // Degrees of tilt from vertical

    // Legacy fields - deprecated but kept for fire effect compatibility
    Vec2 wind;                  // DEPRECATED: minimal values for compatibility
    float stoke;                // DEPRECATED: minimal values for compatibility
    float motionIntensity;      // Overall motion level (0-1)
    bool isStationary;          // True if torch is relatively still
};

struct MotionConfig {
    float tauLP = 0.12f;        // Low-pass time constant for gravity estimation
    float kAccel = 0.1f;        // Reduced sensitivity for legacy compatibility
    float kStoke = 0.01f;       // Reduced sensitivity for legacy compatibility
};
```

**Simplified Implementation:**
- **Gravity Estimation:** Low-pass filter on accelerometer when motion is reasonable (0.8-1.2G)
- **Orientation Calculation:** Normalized gravity vector provides "up" direction
- **Tilt Detection:** Angle between up vector and Z-axis
- **Motion Detection:** Linear acceleration magnitude + gyroscope magnitude
- **Legacy Compatibility:** Minimal wind/stoke values for existing fire effect

**Key Simplifications Made:**
- Removed complex torch physics (~300 lines of code)
- Removed wind simulation and fluid dynamics
- Removed centrifugal and Coriolis effects
- Removed motion history buffers
- Removed velocity integration
- Streamlined to essential orientation and basic motion detection

**Working Cylinder Orientation:**
- Correctly detects when cylinder is upright vs tilted/on-side
- Provides smooth up vector for fire heat propagation direction
- Inverted Z-axis handling for upside-down mounting: `fire.setUpVector(m.up.x, m.up.y, -m.up.z)`

---

## System Integration Patterns

### Main Loop Structure (fire-totem.ino) - Current Implementation
```cpp
void loop() {
    // 1. Timing and frame rate control
    float dt = calculateDeltaTime();  // Clamped to reasonable range

    // 2. Audio processing
    mic.update(dt);                   // Audio analysis with adaptive gain

    // 3. Conditional IMU updates (console-controlled)
    if (imu.isReady() && (console.motionEnabled || console.heatVizEnabled)) {
        imu.updateMotion(dt);         // Basic motion tracking
        imu.updateIMUData();          // Clean IMU data for debugging

        if (console.motionEnabled) {
            // Simple orientation: gravity-based heat rising direction
            // Note: Inverted Z-axis for upside-down mounting
            fire.setUpVector(m.up.x, m.up.y, -m.up.z);
        }
    }

    // 4. Visualization mode handling
    if (console.imuVizEnabled) {
        console.renderIMUVisualization();     // IMU debug visualization
    } else if (console.heatVizEnabled) {
        // Fire + cylinder top indicator
        fire.update(energy, hit);
        fire.render();
        console.renderTopVisualization();
    } else {
        // Normal fire mode
        fire.update(energy, hit);
        fire.show();
    }

    // 5. Serial console processing
    console.update();
}
```

### Parameter Tuning & Debugging System
**SerialConsole.cpp** provides runtime parameter adjustment and visualization modes:

**Fire Parameters:**
```cpp
"fire cooling 85" → fire.params.baseCooling = 85
"fire sparkchance 0.32" → fire.params.sparkChance = 0.32f
"fire reset" → restore all fire defaults
```

**Audio Parameters:**
```cpp
"mic gate 0.06" → mic.noiseGate = 0.06f
"mic attack 0.08" → mic.attackSeconds = 0.08f
"mic gain 1.35" → mic.globalGain = 1.35f
"mic debug on" → enable real-time audio debug output
```

**IMU Debugging & Visualization:**
```cpp
"imu stats" → display current IMU orientation data
"imu debug on" → enable real-time IMU debug output
"imu viz" → toggle IMU visualization on LED matrix
"heat viz" → toggle cylinder top column visualization
"motion on/off" → enable/disable motion effects
```

**Visualization Modes:**
- **IMU Visualization:** Shows orientation directly on LED matrix
- **Heat Visualization:** Shows cylinder top column detection with fire
- **Normal Mode:** Standard fire effect with optional motion

### Error Handling Patterns
- **Sensor Failure:** Graceful degradation (fire continues without IMU/mic)
- **Memory Allocation:** Explicit null checks and cleanup
- **Parameter Bounds:** Consistent use of constrain() for safety
- **NaN/Infinity:** isfinite() checks on sensor data

## Performance & Memory Analysis (Post-Cleanup)

### Current Performance Characteristics
1. **FireEffect::propagateUp()** - Still the main computational cost (heat simulation)
2. **FireEffect::coolCells()** - Random number generation per pixel
3. **AdaptiveMic frequency analysis** - Optimized time-domain spectral approximation
4. **IMU processing** - Significantly reduced CPU load after cleanup

### Memory Usage (Post-Cleanup)
```cpp
FireEffect:     ~1KB (heat buffer: WIDTH*HEIGHT*sizeof(float))
AdaptiveMic:    ~1KB (frequency analysis buffers)
IMUHelper:      ~200B (simplified, no motion history)
SerialConsole:  ~100B (debugging state)
BatteryMonitor: ~50B (configuration and state)
Stack Usage:    ~300B (estimated, reduced complexity)
Total:          ~2.7KB of 256KB RAM (excellent margin)
```

### Performance Improvements from Cleanup
- **Removed ~300 lines** of complex motion physics calculations
- **Eliminated motion history buffers** (8 samples × multiple vectors)
- **Simplified IMU update loop** (basic orientation only)
- **Reduced memory fragmentation** (fewer dynamic allocations)
- **More predictable timing** (fewer conditional branches in motion code)

## Hardware-Specific Constraints

### Seeed XIAO BLE Sense nRF52840 Specifications
- **Flash:** 1MB (plenty for current code ~50KB used)
- **RAM:** 256KB (excellent margin with ~2.7KB used)
- **CPU:** 64MHz ARM Cortex-M4 with FPU (sufficient for real-time processing)
- **Floating Point:** Hardware FPU available (good for fire simulation and IMU processing)

### LED Matrix Constraints
- **Power:** 128 LEDs × 60mA = 7.68A max (need power management)
- **Data Rate:** 800kHz WS2812 protocol
- **Update Time:** ~4ms for full 128-pixel update
- **Color Accuracy:** 8-bit per channel (sufficient for fire effects)

### Sensor Characteristics
- **IMU Sample Rate:** Up to 6.66kHz (using much less)
- **Mic Sample Rate:** 16kHz PDM (good for audio analysis)
- **IMU Noise:** ±0.1 m/s² accel, ±0.1°/s gyro (acceptable)
- **Latency:** ~16ms sensor-to-LED (good for interactive feel)

## Extension Opportunities

### Near-Term Improvements
- **Preset System:** Save/load parameter configurations
- **WiFi Integration:** Remote control and monitoring
- **Additional Effects:** Lightning, ember shower, color variations
- **Battery Optimization:** Sleep modes and power management

### Advanced Features
- **Machine Learning:** Gesture recognition from IMU data
- **Mesh Networking:** Multiple torches synchronized
- **Computer Vision:** Camera-based motion tracking
- **Advanced Audio:** FFT-based frequency analysis

### Code Quality Improvements
- **Unit Tests:** Especially for physics calculations
- **Documentation:** API documentation and usage examples
- **Profiling:** Detailed performance measurement
- **Static Analysis:** Memory safety and code quality tools

## Known Issues & Limitations (Post-Cleanup)

### Current Issues
- **IMU Settling Time:** Gravity estimation requires ~2-3 seconds to stabilize
- **Manual Parameter Tuning:** Audio and fire parameters need environment-specific adjustment
- **Orientation Mounting:** Requires Z-axis inversion for upside-down mounting

### Design Decisions & Limitations
- **Simplified Motion Physics:** Deliberately removed complex wind/centrifugal effects for stability
- **Fixed Resolution:** 16x8 hardcoded throughout codebase (could be parameterized)
- **Legacy Compatibility:** Maintains deprecated wind/stoke fields for fire effect compatibility
- **Basic Motion Detection:** Simple threshold-based motion detection (could be more sophisticated)

## Testing & Validation Notes (Post-Cleanup)

### Currently Working Features
- **Fire Simulation:** Heat-based fire effect with cooling, sparks, and propagation
- **Audio Reactivity:** Music beats trigger flame transients, adaptive gain control
- **Cylinder Orientation:** Correctly detects upright vs tilted/sideways positioning
- **Visualization Modes:** IMU debug visualization and cylinder top column detection
- **Serial Console:** Real-time parameter tuning and debugging interface

### Test Configurations
```cpp
// Standard fire parameters (TotemDefaults.h)
baseCooling = 85, sparkChance = 0.32f
noiseGate = 0.06f, globalGain = 1.35f

// IMU orientation (simplified)
tauLP = 0.12f (gravity estimation time constant)
Alpha = 0.3f (responsive orientation updates)

// Console debugging modes
"imu viz" - orientation visualization on LED matrix
"heat viz" - cylinder top column detection with fire
"motion on/off" - toggle motion effects
```

### Performance Benchmarks (Post-Cleanup)
- **Frame Rate:** Stable ~60Hz (improved from complex physics)
- **Memory Usage:** ~2.7KB of 256KB RAM (excellent headroom)
- **IMU Update Rate:** ~60Hz (much faster than before)
- **Compilation:** Clean compile, no errors after cleanup
- **Startup Time:** <3 seconds for IMU gravity estimation

### Cleanup Success Metrics
- **Code Reduction:** Removed ~300 lines of complex motion physics
- **Compilation Errors:** Fixed all errors from corrupted IMUHelper.cpp
- **Functionality Retained:** Cylinder orientation detection still works
- **Debug Interface:** Enhanced with visualization modes
- **Maintainability:** Simplified codebase much easier to understand and modify

---

*Analysis Date: 2025-01-18*
*Code Version: Post-cleanup with simplified motion physics*
*Major Changes: Removed complex torch physics, streamlined IMU interface, added visualization modes*
*Next Review: After feature additions or if performance issues arise*