# Testing Guide

This guide explains how to use the Blinky Time testing system to prevent regressions during refactoring and ensure code quality.

## 🎯 Testing Philosophy

The testing system is designed to:
- **Prevent regressions** during refactoring and feature development
- **Validate hardware compatibility** across different device configurations
- **Ensure performance standards** for real-time fire effects
- **Maintain code quality** without external dependencies like PlatformIO

## 🧪 Test Types

### 1. Compilation Tests
Verify that code compiles successfully for all device configurations:
- **Hat** (Device Type 1) - 89 LEDs, string configuration
- **Tube Light** (Device Type 2) - 60 LEDs, 4x15 zigzag matrix  
- **Bucket Totem** (Device Type 3) - 128 LEDs, 16x8 matrix

### 2. Unit Tests
Test individual components in isolation:
- **Fire Effect Algorithm** - Heat propagation, cooling, color mapping
- **Audio Processing** - Level detection, smoothing, beat detection
- **Configuration Validation** - Parameter ranges, device settings

### 3. Integration Tests  
Test component interactions and hardware integration:
- **Hardware Abstraction** - LED strips, battery monitoring, IMU
- **Performance Validation** - Frame rate, memory usage, timing
- **System Stability** - Error handling, resource management

### 4. Regression Tests
Critical functionality that must never break:
- **Device Configuration System** - DEVICE_TYPE selection and validation
- **Memory Layout** - Stack usage, heap allocation patterns
- **Hardware Interfaces** - Pin assignments, peripheral initialization

## 🚀 Quick Start

### Method 1: Windows Batch Script (Easiest)
```cmd
# Test all device types (compilation only)
cd tests
test.bat

# Test specific device type
test.bat 2

# Test with hardware validation
test.bat 2 COM4
```

### Method 2: Python Test Runner (Full Features)
```bash
# Install dependencies (optional, for hardware testing)
pip install pyserial

# Run all tests
python tests/run_tests.py

# Test specific device with hardware
python tests/run_tests.py --device 2 --port COM4 --report

# Generate HTML report
python tests/run_tests.py --report
```

### Method 3: Manual Testing (Arduino IDE)
1. Open `tests/test_runner.ino` in Arduino IDE
2. Select **Tools → Board → Seeed XIAO nRF52840 Sense**
3. Upload to your board
4. Open Serial Monitor at 115200 baud
5. View test results in real-time

## 📁 Test Structure

```
tests/
├── BlinkyTest.h              # Lightweight test framework
├── BlinkyTest.cpp            # Framework implementation
├── test_runner.ino           # Main test suite (Arduino sketch)
├── unit/                     # Unit tests
│   ├── test_fire_effect.cpp  # Fire algorithm tests
│   └── test_audio.cpp        # Audio processing tests
├── integration/              # Integration tests
│   └── test_hardware.cpp     # Hardware validation tests
├── run_tests.py              # Python automation script
├── test.bat                  # Windows batch script
├── validate_configs.py       # Configuration validator
└── README.md                 # This file
```

## 🔧 Test Framework Features

### Assertions
```cpp
ASSERT_TRUE(condition);           // Must be true
ASSERT_FALSE(condition);          // Must be false  
ASSERT_EQUAL(expected, actual);   // Values must match
ASSERT_NEAR(val, target, tol);    // Within tolerance
ASSERT_RANGE(val, min, max);      // Within range
```

### Performance Testing
```cpp
BENCHMARK_START();
// ... code to measure ...
BENCHMARK_END("Operation Name", maxMicros);
```

### Hardware Testing
```cpp
ASSERT_PIN_HIGH(pin);             // Pin reads HIGH
ASSERT_PIN_LOW(pin);              // Pin reads LOW
ASSERT_FREE_MEMORY(minBytes);     // Sufficient free memory
```

## 📊 Continuous Integration

### GitHub Actions Workflow
The project includes automated CI/CD pipeline:
- **Compilation Tests** - All device configurations  
- **Code Quality** - Static analysis, formatting checks
- **Documentation** - Required files and completeness
- **Release Validation** - Version consistency, release notes

### Local Pre-commit Testing
Before committing changes:
```bash
# Quick compilation test
tests/test.bat

# Full validation with hardware
python tests/run_tests.py --device all --port COM4 --report

# Configuration validation  
python tests/validate_configs.py
```

## 🎛 Configuration Testing

### Device Configuration Validation
```bash
python tests/validate_configs.py
```

Validates:
- **Required Constants** - LED_COUNT, LED_DATA_PIN, etc.
- **Parameter Ranges** - Safe values for fire effects
- **Hardware Compatibility** - Pin assignments, color orders
- **Matrix Dimensions** - LED count matches matrix size

### Custom Test Configuration
Modify test behavior in `test_runner.ino`:
```cpp
#define TEST_MODE TEST_MODE_UNIT        // Unit tests only
#define TEST_MODE TEST_MODE_INTEGRATION // Hardware tests only  
#define TEST_MODE TEST_MODE_ALL         // Complete suite (default)
```

## 🚨 Troubleshooting

### Compilation Failures
1. **Arduino CLI not found** - Install and add to PATH
2. **Board package missing** - Install Seeeduino nRF52 package  
3. **Library missing** - Install Adafruit NeoPixel library
4. **Device type errors** - Check DEVICE_TYPE in sketch

### Hardware Test Issues
1. **Serial port not found** - Check USB connection and drivers
2. **Upload failures** - Try bootloader mode (double-tap reset)
3. **Test timeouts** - Increase timeout or check serial connection
4. **Memory issues** - Close other serial applications

### Test Framework Issues
1. **Assertion failures** - Check logic and expected values
2. **Performance issues** - Optimize code or adjust benchmarks  
3. **Memory leaks** - Validate allocation/deallocation pairs
4. **Hardware mocking** - Update mock values for testing

## 📈 Performance Standards

### Target Performance Metrics
- **Fire Effect Frame Rate**: 60 FPS minimum
- **Audio Processing**: < 500μs per frame
- **Memory Usage**: < 80% of available RAM
- **Boot Time**: < 3 seconds to first LED output

### Benchmarking
```cpp
TEST_CASE("Fire Effect Performance");
BENCHMARK_START();
// Run one complete fire effect frame
updateFireEffect();
BENCHMARK_END("Fire Frame", 16667); // 60 FPS = 16.67ms max
```

## 🔄 Regression Testing Workflow

### Before Refactoring
1. **Run full test suite** - Establish baseline
2. **Document current behavior** - Note any warnings
3. **Create feature branch** - Isolate changes
4. **Set performance benchmarks** - Record current metrics

### During Refactoring  
1. **Run tests frequently** - Catch issues early
2. **Test incrementally** - Small changes, frequent validation
3. **Monitor performance** - Watch for degradation
4. **Update tests if needed** - Keep tests current with changes

### After Refactoring
1. **Full regression test** - All device types and features
2. **Performance validation** - Compare to baseline
3. **Hardware verification** - Test on actual devices
4. **Documentation update** - Reflect any changes

## 📝 Writing New Tests

### Adding Unit Tests
1. Create test file in `tests/unit/`
2. Include `BlinkyTest.h`
3. Write test functions using assertions
4. Add test runner call in `test_runner.ino`

Example:
```cpp
void testNewFeature() {
  TEST_CASE("New Feature Validation");
  
  // Test setup
  int result = newFeatureFunction(input);
  
  // Assertions
  ASSERT_EQUAL(result, expectedValue);
  ASSERT_RANGE(result, minValue, maxValue);
}
```

### Adding Hardware Tests
1. Create test in `tests/integration/`
2. Use hardware mocking when possible
3. Provide clear failure diagnostics
4. Include performance benchmarks

## 🎯 Best Practices

### Test Design
- **Test one thing** - Single responsibility per test
- **Use descriptive names** - Clear test purpose
- **Provide context** - Explain expected behavior
- **Handle edge cases** - Test boundary conditions

### Performance Testing
- **Set realistic targets** - Based on hardware capabilities
- **Test under load** - Simulate real usage patterns  
- **Monitor memory** - Check for leaks and fragmentation
- **Validate timing** - Real-time constraints

### Hardware Testing
- **Mock when possible** - Reduce hardware dependencies
- **Test error conditions** - Handle hardware failures gracefully
- **Validate configurations** - Ensure safe operation
- **Document setup** - Clear hardware test procedures

## 🤝 Contributing Tests

When contributing new features:

1. **Add corresponding tests** - New code needs test coverage
2. **Update existing tests** - Reflect API changes
3. **Validate on hardware** - Test with actual devices
4. **Document test procedures** - Help others validate changes

See [CONTRIBUTING.md](../CONTRIBUTING.md) for detailed contribution guidelines.

---

**Remember**: Good tests make refactoring safe and enable confident development! 🔥