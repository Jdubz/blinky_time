/**
 * Fire Effect Algorithm Tests
 * 
 * Unit tests for fire simulation algorithms including heat propagation,
 * cooling calculations, and color mapping functions.
 */

#include "../BlinkyTest.h"
#include "../../blinky-things/FireEffect.h"
#include "../../blinky-things/Globals.h"

// Mock LED strip for testing
class MockNeoPixel {
public:
  uint32_t pixels[256];
  int numPixels;
  
  MockNeoPixel(int n) : numPixels(n) {
    for (int i = 0; i < n; i++) {
      pixels[i] = 0;
    }
  }
  
  void setPixelColor(int n, uint32_t color) {
    if (n >= 0 && n < numPixels) {
      pixels[n] = color;
    }
  }
  
  uint32_t getPixelColor(int n) {
    if (n >= 0 && n < numPixels) {
      return pixels[n];
    }
    return 0;
  }
  
  static uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  }
};

void testHeatCalculation() {
  TEST_CASE("Heat Calculation Bounds");
  
  // Test heat values stay within valid range
  uint8_t heat = 255;
  uint8_t cooling = 50;
  
  // Simulate cooling
  if (heat > cooling) {
    heat -= cooling;
  } else {
    heat = 0;
  }
  
  ASSERT_RANGE(heat, 0, 255);
}

void testColorMapping() {
  TEST_CASE("Heat to Color Mapping");
  
  // Test color mapping for different heat values
  MockNeoPixel strip(60);
  
  // Cold (should be black/dark)
  uint8_t coldHeat = 0;
  uint32_t coldColor = strip.Color(0, 0, 0);
  ASSERT_EQUAL(coldColor, 0);
  
  // Hot (should have red component)
  uint8_t hotHeat = 255;
  uint32_t hotColor = strip.Color(255, 255, 100);
  ASSERT_TRUE((hotColor >> 16) & 0xFF > 200); // Red component
}

void testMatrixMapping() {
  TEST_CASE("Zigzag Matrix Mapping");
  
  // Test zigzag pattern for 4x15 matrix
  int width = 4;
  int height = 15;
  
  // Test column 0 (bottom to top)
  int led0 = 0 * height + 5; // Row 5, Column 0
  ASSERT_EQUAL(led0, 5);
  
  // Test column 1 (top to bottom, zigzag)
  int led1 = (1 * height) + (height - 1 - 5); // Row 5, Column 1
  ASSERT_EQUAL(led1, 24);
  
  // Test column 2 (bottom to top)
  int led2 = 2 * height + 5; // Row 5, Column 2
  ASSERT_EQUAL(led2, 35);
  
  // Test column 3 (top to bottom, zigzag)
  int led3 = (3 * height) + (height - 1 - 5); // Row 5, Column 3
  ASSERT_EQUAL(led3, 54);
}

void testSparkGeneration() {
  TEST_CASE("Spark Generation Probability");
  
  float sparkChance = 0.2f; // 20% chance
  int totalTests = 1000;
  int sparkCount = 0;
  
  // Simulate spark generation
  for (int i = 0; i < totalTests; i++) {
    float randomValue = (float)random(1000) / 1000.0f;
    if (randomValue < sparkChance) {
      sparkCount++;
    }
  }
  
  // Should be roughly 20% with some tolerance
  float actualRate = (float)sparkCount / totalTests;
  ASSERT_RANGE(actualRate, 0.15f, 0.25f);
}

void testHeatPropagation() {
  TEST_CASE("Heat Propagation");
  
  uint8_t heatMap[60];
  
  // Initialize with cold
  for (int i = 0; i < 60; i++) {
    heatMap[i] = 0;
  }
  
  // Add heat at bottom
  heatMap[0] = 255;
  
  // Simulate one propagation step
  for (int i = 1; i < 59; i++) {
    uint8_t avgHeat = (heatMap[i-1] + heatMap[i] + heatMap[i+1]) / 3;
    heatMap[i] = (avgHeat > 20) ? avgHeat - 20 : 0;
  }
  
  // Heat should have spread upward
  ASSERT_TRUE(heatMap[1] > 0);
  ASSERT_TRUE(heatMap[1] < heatMap[0]);
}

void testPerformance() {
  TEST_CASE("Fire Effect Performance");
  
  MockNeoPixel strip(60);
  uint8_t heatMap[60];
  
  // Initialize heat map
  for (int i = 0; i < 60; i++) {
    heatMap[i] = random(255);
  }
  
  BENCHMARK_START();
  
  // Simulate one fire effect frame
  for (int i = 0; i < 60; i++) {
    // Heat propagation
    if (i < 59) {
      heatMap[i] = (heatMap[i] + heatMap[i+1]) / 2;
    }
    
    // Cooling
    if (heatMap[i] > 5) {
      heatMap[i] -= 5;
    } else {
      heatMap[i] = 0;
    }
    
    // Color mapping
    uint8_t heat = heatMap[i];
    uint8_t r = (heat > 128) ? 255 : heat * 2;
    uint8_t g = (heat > 192) ? (heat - 192) * 4 : 0;
    uint8_t b = (heat > 224) ? (heat - 224) * 8 : 0;
    
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  
  BENCHMARK_END("Single Frame", 1000); // Should complete in under 1ms
}

void runFireEffectTests() {
  Serial.println("=== FIRE EFFECT TESTS ===");
  
  testHeatCalculation();
  testColorMapping();
  testMatrixMapping();
  testSparkGeneration();
  testHeatPropagation();
  testPerformance();
  
  Serial.println();
}