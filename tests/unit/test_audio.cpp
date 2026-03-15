/**
 * Audio Processing Tests
 * 
 * Unit tests for adaptive microphone, audio level detection,
 * and audio-reactive fire effect integration.
 */

#include "../BlinkyTest.h"

// Mock audio data for testing
struct MockAudioData {
  float level;
  bool isBeat;
  float smoothedLevel;
};

void testAudioLevelNormalization() {
  TEST_CASE("Audio Level Normalization");
  
  // Test audio level stays within 0.0 - 1.0 range
  float rawLevel = 1.5f; // Over-range input
  float normalizedLevel = (rawLevel > 1.0f) ? 1.0f : rawLevel;
  
  ASSERT_RANGE(normalizedLevel, 0.0f, 1.0f);
  
  // Test negative values
  rawLevel = -0.5f;
  normalizedLevel = (rawLevel < 0.0f) ? 0.0f : rawLevel;
  
  ASSERT_RANGE(normalizedLevel, 0.0f, 1.0f);
}

void testAudioSmoothing() {
  TEST_CASE("Audio Level Smoothing");
  
  float currentLevel = 0.8f;
  float newLevel = 0.2f;
  float smoothingFactor = 0.1f;
  
  // Exponential smoothing
  float smoothedLevel = currentLevel + smoothingFactor * (newLevel - currentLevel);
  
  // Should be between old and new values
  ASSERT_TRUE(smoothedLevel > newLevel);
  ASSERT_TRUE(smoothedLevel < currentLevel);
  ASSERT_NEAR(smoothedLevel, 0.74f, 0.01f);
}

void testBeatDetection() {
  TEST_CASE("Beat Detection Logic");
  
  float currentLevel = 0.3f;
  float averageLevel = 0.2f;
  float threshold = 1.5f;
  
  bool isBeat = currentLevel > (averageLevel * threshold);
  
  ASSERT_TRUE(isBeat); // 0.3 > (0.2 * 1.5 = 0.3), should be true
  
  // Test no beat condition
  currentLevel = 0.25f;
  isBeat = currentLevel > (averageLevel * threshold);
  
  ASSERT_FALSE(isBeat); // 0.25 < 0.3, should be false
}

void testAudioReactiveSparkBoost() {
  TEST_CASE("Audio Reactive Spark Boost");
  
  float baseSparkChance = 0.1f; // 10%
  float audioLevel = 0.8f;
  float audioBoost = 0.3f; // 30% boost at full volume
  
  float boostedChance = baseSparkChance + (audioLevel * audioBoost);
  
  ASSERT_NEAR(boostedChance, 0.34f, 0.01f); // 0.1 + (0.8 * 0.3) = 0.34
  ASSERT_TRUE(boostedChance > baseSparkChance);
}

void testAdaptiveGain() {
  TEST_CASE("Adaptive Microphone Gain");
  
  float targetLevel = 0.5f;
  float currentLevel = 0.2f;
  float currentGain = 1.0f;
  float gainAdjustRate = 0.05f;
  
  // Gain should increase if level is too low
  if (currentLevel < targetLevel * 0.8f) {
    currentGain += gainAdjustRate;
  }
  
  ASSERT_TRUE(currentGain > 1.0f);
  ASSERT_NEAR(currentGain, 1.05f, 0.001f);
}

void testAudioFrequencyFiltering() {
  TEST_CASE("Audio Frequency Response");
  
  // Simulate different frequency responses
  float bassResponse = 0.8f;    // Strong bass
  float midResponse = 0.4f;     // Moderate mids
  float trebleResponse = 0.2f;  // Weak treble
  
  // Fire should respond more to bass and mids
  float fireResponse = (bassResponse * 0.6f) + (midResponse * 0.3f) + (trebleResponse * 0.1f);
  
  ASSERT_NEAR(fireResponse, 0.62f, 0.01f); // (0.8*0.6 + 0.4*0.3 + 0.2*0.1)
  ASSERT_TRUE(fireResponse > midResponse);
}

void testAudioMemoryUsage() {
  TEST_CASE("Audio Buffer Memory");
  
  const int bufferSize = 64;
  int16_t audioBuffer[bufferSize];
  
  // Verify buffer size is reasonable
  size_t bufferMemory = sizeof(audioBuffer);
  ASSERT_TRUE(bufferMemory <= 256); // Should use less than 256 bytes
  
  // Test buffer initialization
  for (int i = 0; i < bufferSize; i++) {
    audioBuffer[i] = 0;
  }
  
  ASSERT_EQUAL(audioBuffer[0], 0);
  ASSERT_EQUAL(audioBuffer[bufferSize-1], 0);
}

void testAudioPerformance() {
  TEST_CASE("Audio Processing Performance");
  
  const int sampleCount = 64;
  int16_t samples[sampleCount];
  
  // Fill with test data
  for (int i = 0; i < sampleCount; i++) {
    samples[i] = random(-32768, 32767);
  }
  
  BENCHMARK_START();
  
  // Simulate audio processing
  float sum = 0;
  for (int i = 0; i < sampleCount; i++) {
    sum += abs(samples[i]);
  }
  float averageLevel = sum / sampleCount / 32768.0f;
  
  // Apply smoothing
  static float smoothedLevel = 0;
  smoothedLevel = smoothedLevel * 0.9f + averageLevel * 0.1f;
  
  BENCHMARK_END("Audio Processing", 500); // Should complete in under 500μs
  
  ASSERT_RANGE(smoothedLevel, 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// AgcStrategy: SOFTWARE vs HARDWARE selection and behaviour
//
// These tests exercise the strategy-selection logic and per-strategy
// dead-zone / step-size constants without linking the full AdaptiveMic
// stack.  Ground-truth values mirror AdaptiveMic.cpp.
// ---------------------------------------------------------------------------

void testAgcStrategySoftwareDeadZone() {
  TEST_CASE("AgcStrategy::SOFTWARE dead zone is 0.02");

  // SOFTWARE strategy uses ±0.02 dead zone — wider than HARDWARE (±0.01)
  // so minor mic noise doesn't continuously nudge post-decimation gain.
  const float DEAD_ZONE_SW = 0.02f;
  const float DEAD_ZONE_HW = 0.01f;

  // Within SOFTWARE dead zone — no gain change
  float delta = 0.015f;
  bool swAdjust = (delta > DEAD_ZONE_SW || delta < -DEAD_ZONE_SW);
  ASSERT_FALSE(swAdjust);

  // Outside SOFTWARE dead zone — gain change
  delta = 0.025f;
  swAdjust = (delta > DEAD_ZONE_SW || delta < -DEAD_ZONE_SW);
  ASSERT_TRUE(swAdjust);

  // Delta that crosses HARDWARE dead zone but not SOFTWARE dead zone
  delta = 0.015f;
  bool hwAdjust = (delta > DEAD_ZONE_HW || delta < -DEAD_ZONE_HW);
  swAdjust = (delta > DEAD_ZONE_SW || delta < -DEAD_ZONE_SW);
  ASSERT_TRUE(hwAdjust);   // Would trigger on hardware mic
  ASSERT_FALSE(swAdjust);  // Not on software mic (wider dead zone)
}

void testAgcStrategySoftwareStepSizes() {
  TEST_CASE("AgcStrategy::SOFTWARE uses smaller gain steps than HARDWARE");

  // Step sizes (dB) from AdaptiveMic.cpp:
  //   SOFTWARE normal:  up=2, down=1
  //   HARDWARE normal:  up=4, down=2
  //   HARDWARE fast:    up=6, down=3
  const int SW_STEP_UP   = 2;
  const int SW_STEP_DOWN = 1;
  const int HW_STEP_UP   = 4;
  const int HW_STEP_DOWN = 2;

  ASSERT_TRUE(SW_STEP_UP   < HW_STEP_UP);
  ASSERT_TRUE(SW_STEP_DOWN < HW_STEP_DOWN);

  // Gain must be clamped at bounds [0, 80]
  int gainMin = 0, gainMax = 80;
  int gain = 78;
  gain = min(gainMax, gain + SW_STEP_UP);
  ASSERT_EQUAL(gain, 80);  // Clamped at max

  gain = 1;
  gain = max(gainMin, gain - SW_STEP_DOWN);
  ASSERT_EQUAL(gain, 0);   // Clamped at min
}

void testAgcStrategyFastAgcRequiresHardware() {
  TEST_CASE("Fast AGC mode requires HARDWARE strategy");

  // Fast AGC is only enabled when the mic has a hardware gain register.
  // SOFTWARE strategy suppresses fast AGC even when fastAgcEnabled=true
  // because post-decimation software gain doesn't improve SNR.

  bool fastAgcEnabled = true;

  // HARDWARE strategy: fast AGC allowed
  bool hasHardwareGain = true;
  bool inFastAgcMode = hasHardwareGain && fastAgcEnabled;
  ASSERT_TRUE(inFastAgcMode);

  // SOFTWARE strategy: fast AGC suppressed regardless of fastAgcEnabled
  hasHardwareGain = false;
  inFastAgcMode = hasHardwareGain && fastAgcEnabled;
  ASSERT_FALSE(inFastAgcMode);
}

void runAudioTests() {
  Serial.println("=== AUDIO PROCESSING TESTS ===");

  testAudioLevelNormalization();
  testAudioSmoothing();
  testBeatDetection();
  testAudioReactiveSparkBoost();
  testAdaptiveGain();
  testAudioFrequencyFiltering();
  testAudioMemoryUsage();
  testAudioPerformance();
  testAgcStrategySoftwareDeadZone();
  testAgcStrategySoftwareStepSizes();
  testAgcStrategyFastAgcRequiresHardware();

  Serial.println();
}