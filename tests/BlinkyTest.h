/**
 * Blinky Time Test Framework
 * 
 * A lightweight testing framework for Arduino-based LED controllers.
 * Provides unit testing, integration testing, and hardware validation
 * without external dependencies like PlatformIO.
 * 
 * Author: Blinky Time Project Contributors
 * License: Creative Commons Attribution-ShareAlike 4.0 International
 */

#ifndef BLINKY_TEST_H
#define BLINKY_TEST_H

#include <Arduino.h>

// Test result tracking
struct TestResults {
  int totalTests = 0;
  int passedTests = 0;
  int failedTests = 0;
  unsigned long startTime = 0;
  unsigned long endTime = 0;
};

// Global test results
extern TestResults testResults;

// Test macros
#define TEST_BEGIN() \
  testResults = TestResults(); \
  testResults.startTime = millis(); \
  Serial.println("=== BLINKY TIME TEST SUITE ==="); \
  Serial.println();

#define TEST_END() \
  testResults.endTime = millis(); \
  Serial.println(); \
  Serial.println("=== TEST SUMMARY ==="); \
  Serial.print("Total Tests: "); Serial.println(testResults.totalTests); \
  Serial.print("Passed: "); Serial.println(testResults.passedTests); \
  Serial.print("Failed: "); Serial.println(testResults.failedTests); \
  Serial.print("Success Rate: "); Serial.print((float)testResults.passedTests / testResults.totalTests * 100, 1); Serial.println("%"); \
  Serial.print("Duration: "); Serial.print(testResults.endTime - testResults.startTime); Serial.println("ms"); \
  Serial.println(); \
  if (testResults.failedTests == 0) { \
    Serial.println("✅ ALL TESTS PASSED!"); \
  } else { \
    Serial.println("❌ SOME TESTS FAILED!"); \
  }

#define TEST_CASE(name) \
  Serial.print("Testing: "); Serial.print(name); Serial.print("... "); \
  testResults.totalTests++;

#define ASSERT_TRUE(condition) \
  if (condition) { \
    Serial.println("PASS"); \
    testResults.passedTests++; \
  } else { \
    Serial.println("FAIL"); \
    Serial.print("  Expected: true, Got: false at line "); Serial.println(__LINE__); \
    testResults.failedTests++; \
  }

#define ASSERT_FALSE(condition) \
  if (!(condition)) { \
    Serial.println("PASS"); \
    testResults.passedTests++; \
  } else { \
    Serial.println("FAIL"); \
    Serial.print("  Expected: false, Got: true at line "); Serial.println(__LINE__); \
    testResults.failedTests++; \
  }

#define ASSERT_EQUAL(expected, actual) \
  if ((expected) == (actual)) { \
    Serial.println("PASS"); \
    testResults.passedTests++; \
  } else { \
    Serial.println("FAIL"); \
    Serial.print("  Expected: "); Serial.print(expected); \
    Serial.print(", Got: "); Serial.print(actual); \
    Serial.print(" at line "); Serial.println(__LINE__); \
    testResults.failedTests++; \
  }

#define ASSERT_NOT_EQUAL(expected, actual) \
  if ((expected) != (actual)) { \
    Serial.println("PASS"); \
    testResults.passedTests++; \
  } else { \
    Serial.println("FAIL"); \
    Serial.print("  Expected: not "); Serial.print(expected); \
    Serial.print(", Got: "); Serial.print(actual); \
    Serial.print(" at line "); Serial.println(__LINE__); \
    testResults.failedTests++; \
  }

#define ASSERT_NEAR(expected, actual, tolerance) \
  if (abs((expected) - (actual)) <= (tolerance)) { \
    Serial.println("PASS"); \
    testResults.passedTests++; \
  } else { \
    Serial.println("FAIL"); \
    Serial.print("  Expected: "); Serial.print(expected); \
    Serial.print(" ± "); Serial.print(tolerance); \
    Serial.print(", Got: "); Serial.print(actual); \
    Serial.print(" at line "); Serial.println(__LINE__); \
    testResults.failedTests++; \
  }

#define ASSERT_RANGE(value, min, max) \
  if ((value) >= (min) && (value) <= (max)) { \
    Serial.println("PASS"); \
    testResults.passedTests++; \
  } else { \
    Serial.println("FAIL"); \
    Serial.print("  Expected: "); Serial.print(min); \
    Serial.print(" <= "); Serial.print(value); \
    Serial.print(" <= "); Serial.print(max); \
    Serial.print(" at line "); Serial.println(__LINE__); \
    testResults.failedTests++; \
  }

// Hardware testing utilities
#define ASSERT_PIN_HIGH(pin) \
  pinMode(pin, INPUT); \
  ASSERT_TRUE(digitalRead(pin) == HIGH)

#define ASSERT_PIN_LOW(pin) \
  pinMode(pin, INPUT); \
  ASSERT_TRUE(digitalRead(pin) == LOW)

// Performance testing
#define BENCHMARK_START() unsigned long _benchmark_start = micros();
#define BENCHMARK_END(name, maxMicros) \
  unsigned long _benchmark_duration = micros() - _benchmark_start; \
  Serial.print("Benchmark "); Serial.print(name); \
  Serial.print(": "); Serial.print(_benchmark_duration); Serial.println("μs"); \
  ASSERT_TRUE(_benchmark_duration <= maxMicros)

// Memory testing
#define ASSERT_FREE_MEMORY(minBytes) \
  ASSERT_TRUE(getFreeMemory() >= minBytes)

// Function to get free memory (platform specific)
#ifdef __arm__
extern "C" char* sbrk(int incr);
static int getFreeMemory() {
  char top;
  return &top - reinterpret_cast<char*>(sbrk(0));
}
#else
static int getFreeMemory() {
  return 1024; // Dummy value for non-ARM platforms
}
#endif

#endif // BLINKY_TEST_H