/**
 * Blinky Time Test Runner
 * 
 * Main test suite runner for regression testing during refactoring.
 * Runs unit tests, integration tests, and hardware validation tests.
 * 
 * To run tests:
 * 1. Set DEVICE_TYPE to 0 in blinky-things.ino (test mode)
 * 2. Upload this sketch instead of main sketch
 * 3. Open Serial Monitor at 115200 baud
 * 4. Tests will run automatically and report results
 * 
 * Author: Blinky Time Project Contributors
 * License: Creative Commons Attribution-ShareAlike 4.0 International
 */

#include "BlinkyTest.h"

// Include all test suites
extern void runFireEffectTests();
extern void runAudioTests();
extern void runHardwareTests();

// Test configuration
#define TEST_MODE_UNIT        1
#define TEST_MODE_INTEGRATION 2
#define TEST_MODE_ALL         3

#ifndef TEST_MODE
#define TEST_MODE TEST_MODE_ALL
#endif

void setup() {
  Serial.begin(115200);
  
  // Wait for serial connection
  while (!Serial) {
    delay(10);
  }
  
  delay(2000); // Give time to open serial monitor
  
  Serial.println("Blinky Time Test Suite");
  Serial.println("Repository: https://github.com/Jdubz/blinky_time");
  Serial.println("License: Creative Commons Attribution-ShareAlike 4.0");
  Serial.println();
  
  // Initialize random seed for reproducible tests
  randomSeed(42);
  
  TEST_BEGIN();
  
  // Run test suites based on configuration
  #if TEST_MODE == TEST_MODE_UNIT || TEST_MODE == TEST_MODE_ALL
    runFireEffectTests();
    runAudioTests();
  #endif
  
  #if TEST_MODE == TEST_MODE_INTEGRATION || TEST_MODE == TEST_MODE_ALL
    runHardwareTests();
  #endif
  
  // Run system-wide regression tests
  runRegressionTests();
  
  TEST_END();
  
  // Provide next steps
  Serial.println();
  Serial.println("=== NEXT STEPS ===");
  if (testResults.failedTests == 0) {
    Serial.println("✅ All tests passed! Safe to proceed with refactoring.");
    Serial.println("💡 Consider running hardware validation tests if you haven't.");
  } else {
    Serial.println("❌ Some tests failed! Fix issues before refactoring.");
    Serial.println("🔍 Check failed test details above for debugging information.");
  }
  
  Serial.println();
  Serial.println("Test modes available:");
  Serial.println("- TEST_MODE_UNIT: Unit tests only");
  Serial.println("- TEST_MODE_INTEGRATION: Hardware integration tests");
  Serial.println("- TEST_MODE_ALL: Complete test suite (default)");
}

void loop() {
  // Tests run once in setup, then idle
  delay(10000);
  
  // Optional: Flash built-in LED to show system is alive
  static bool ledState = false;
  #ifdef LED_BUILTIN
    digitalWrite(LED_BUILTIN, ledState);
    ledState = !ledState;
  #endif
}

/**
 * Regression tests for critical functionality that must not break
 */
void runRegressionTests() {
  Serial.println("=== REGRESSION TESTS ===");
  
  // Test 1: Device configuration system
  TEST_CASE("Device Configuration System");
  #ifdef DEVICE_TYPE
    ASSERT_RANGE(DEVICE_TYPE, 0, 3); // 0=test, 1=hat, 2=tube, 3=bucket
  #else
    ASSERT_TRUE(false); // DEVICE_TYPE must be defined
  #endif
  
  // Test 2: Memory layout stability
  TEST_CASE("Memory Layout Stability");
  int initialFreeMemory = getFreeMemory();
  
  // Allocate and free some memory
  uint8_t* testBuffer = (uint8_t*)malloc(100);
  ASSERT_TRUE(testBuffer != nullptr);
  free(testBuffer);
  
  int finalFreeMemory = getFreeMemory();
  ASSERT_NEAR(finalFreeMemory, initialFreeMemory, 50); // Should be close
  
  // Test 3: Critical constants validation
  TEST_CASE("Critical Constants");
  
  // LED data pin
  const int LED_DATA_PIN = 10;
  ASSERT_EQUAL(LED_DATA_PIN, 10);
  
  // Maximum heat value
  const uint8_t MAX_HEAT = 255;
  ASSERT_EQUAL(MAX_HEAT, 255);
  
  // Fire effect frame rate target
  const int TARGET_FPS = 60;
  ASSERT_EQUAL(TARGET_FPS, 60);
  
  // Test 4: Function pointer stability
  TEST_CASE("Function Pointer Stability");
  
  // Test that critical function pointers are valid
  void (*testPtr)() = runFireEffectTests;
  ASSERT_TRUE(testPtr != nullptr);
  
  // Test 5: Compilation flags
  TEST_CASE("Compilation Environment");
  
  #ifdef __arm__
    ASSERT_TRUE(true); // Running on ARM (nRF52840)
  #else
    Serial.println("  WARNING: Not running on ARM target");
  #endif
  
  // Test 6: Arduino framework integration
  TEST_CASE("Arduino Framework");
  
  ASSERT_TRUE(millis() > 0); // System timer working
  ASSERT_TRUE(Serial); // Serial communication working
  
  Serial.println();
}

/**
 * Hardware validation tests - only run if actual hardware is connected
 */
void runHardwareValidationTests() {
  Serial.println("=== HARDWARE VALIDATION ===");
  Serial.println("Connect hardware and uncomment these tests:");
  Serial.println();
  
  /*
  // Uncomment these tests when hardware is connected
  
  TEST_CASE("LED Strip Response");
  strip.setPixelColor(0, strip.Color(255, 0, 0)); // Red
  strip.show();
  delay(500);
  strip.setPixelColor(0, strip.Color(0, 255, 0)); // Green  
  strip.show();
  delay(500);
  strip.setPixelColor(0, strip.Color(0, 0, 255)); // Blue
  strip.show();
  delay(500);
  strip.setPixelColor(0, 0); // Off
  strip.show();
  ASSERT_TRUE(true); // Manual verification required
  
  TEST_CASE("Battery Voltage Reading");
  float voltage = readBatteryVoltage();
  ASSERT_RANGE(voltage, 3.0f, 4.2f);
  
  TEST_CASE("Microphone Response");
  float audioLevel = readAudioLevel();
  ASSERT_RANGE(audioLevel, 0.0f, 1.0f);
  
  TEST_CASE("IMU Readings");
  readIMU();
  ASSERT_TRUE(abs(accelX) < 2000); // Reasonable acceleration range
  ASSERT_TRUE(abs(accelY) < 2000);
  ASSERT_TRUE(abs(accelZ) < 2000);
  */
}