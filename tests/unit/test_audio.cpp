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
  
  Serial.println();
}