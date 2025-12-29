/**
 * Preset Manager Tests
 *
 * Unit tests for audio parameter presets including:
 * - Preset name parsing
 * - Preset application
 * - Preset parameter validation
 */

#include "../BlinkyTest.h"
#include "../../blinky-things/config/Presets.h"

// Forward declarations for tests that don't need full objects
void testPresetNameParsing() {
  TEST_CASE("Preset Name Parsing - Valid Names");

  // Test all valid preset names (case-insensitive)
  ASSERT_EQUAL(static_cast<uint8_t>(PresetId::DEFAULT),
               static_cast<uint8_t>(PresetManager::parsePresetName("default")));
}

void testPresetNameParsingQuiet() {
  TEST_CASE("Preset Name Parsing - Quiet");

  ASSERT_EQUAL(static_cast<uint8_t>(PresetId::QUIET),
               static_cast<uint8_t>(PresetManager::parsePresetName("quiet")));
}

void testPresetNameParsingLoud() {
  TEST_CASE("Preset Name Parsing - Loud");

  ASSERT_EQUAL(static_cast<uint8_t>(PresetId::LOUD),
               static_cast<uint8_t>(PresetManager::parsePresetName("loud")));
}

void testPresetNameParsingLive() {
  TEST_CASE("Preset Name Parsing - Live");

  ASSERT_EQUAL(static_cast<uint8_t>(PresetId::LIVE),
               static_cast<uint8_t>(PresetManager::parsePresetName("live")));
}

void testPresetNameParsingCaseInsensitive() {
  TEST_CASE("Preset Name Parsing - Case Insensitive");

  // Test case insensitivity
  ASSERT_EQUAL(static_cast<uint8_t>(PresetId::QUIET),
               static_cast<uint8_t>(PresetManager::parsePresetName("QUIET")));
}

void testPresetNameParsingMixedCase() {
  TEST_CASE("Preset Name Parsing - Mixed Case");

  ASSERT_EQUAL(static_cast<uint8_t>(PresetId::LOUD),
               static_cast<uint8_t>(PresetManager::parsePresetName("Loud")));
}

void testPresetNameParsingInvalid() {
  TEST_CASE("Preset Name Parsing - Invalid Name");

  // Test invalid preset names
  ASSERT_EQUAL(static_cast<uint8_t>(PresetId::NUM_PRESETS),
               static_cast<uint8_t>(PresetManager::parsePresetName("invalid")));
}

void testPresetNameParsingEmpty() {
  TEST_CASE("Preset Name Parsing - Empty String");

  ASSERT_EQUAL(static_cast<uint8_t>(PresetId::NUM_PRESETS),
               static_cast<uint8_t>(PresetManager::parsePresetName("")));
}

void testPresetNameParsingNull() {
  TEST_CASE("Preset Name Parsing - Null");

  ASSERT_EQUAL(static_cast<uint8_t>(PresetId::NUM_PRESETS),
               static_cast<uint8_t>(PresetManager::parsePresetName(nullptr)));
}

void testPresetNameRetrieval() {
  TEST_CASE("Preset Name Retrieval");

  // Test getting preset names
  ASSERT_TRUE(strcmp(PresetManager::getPresetName(PresetId::DEFAULT), "default") == 0);
}

void testPresetNameRetrievalQuiet() {
  TEST_CASE("Preset Name Retrieval - Quiet");

  ASSERT_TRUE(strcmp(PresetManager::getPresetName(PresetId::QUIET), "quiet") == 0);
}

void testPresetNameRetrievalLoud() {
  TEST_CASE("Preset Name Retrieval - Loud");

  ASSERT_TRUE(strcmp(PresetManager::getPresetName(PresetId::LOUD), "loud") == 0);
}

void testPresetNameRetrievalLive() {
  TEST_CASE("Preset Name Retrieval - Live");

  ASSERT_TRUE(strcmp(PresetManager::getPresetName(PresetId::LIVE), "live") == 0);
}

void testPresetNameRetrievalInvalid() {
  TEST_CASE("Preset Name Retrieval - Invalid ID");

  // Test invalid ID returns "unknown"
  ASSERT_TRUE(strcmp(PresetManager::getPresetName(PresetId::NUM_PRESETS), "unknown") == 0);
}

void testPresetCount() {
  TEST_CASE("Preset Count");

  // Should have 4 presets
  ASSERT_EQUAL(4, PresetManager::getPresetCount());
}

void testPresetParamsExist() {
  TEST_CASE("Preset Params - All Presets Have Valid Params");

  // All presets should have valid parameter sets
  for (uint8_t i = 0; i < PresetManager::getPresetCount(); i++) {
    const PresetParams* params = PresetManager::getPresetParams(static_cast<PresetId>(i));
    ASSERT_TRUE(params != nullptr);
  }
}

void testPresetParamsInvalidId() {
  TEST_CASE("Preset Params - Invalid ID Returns Null");

  const PresetParams* params = PresetManager::getPresetParams(PresetId::NUM_PRESETS);
  ASSERT_TRUE(params == nullptr);
}

void testPresetParamsRanges() {
  TEST_CASE("Preset Params - Valid Ranges");

  // Check that all preset parameters are within valid ranges
  for (uint8_t i = 0; i < PresetManager::getPresetCount(); i++) {
    const PresetParams* p = PresetManager::getPresetParams(static_cast<PresetId>(i));
    if (p != nullptr) {
      // hitthresh: 1.5f - 10.0f (registered range)
      ASSERT_TRUE(p->hitthresh >= 1.5f && p->hitthresh <= 10.0f);

      // attackmult: 1.1f - 2.0f
      ASSERT_TRUE(p->attackmult >= 1.1f && p->attackmult <= 2.0f);

      // avgtau: 0.1f - 5.0f
      ASSERT_TRUE(p->avgtau >= 0.1f && p->avgtau <= 5.0f);

      // cooldown: 20 - 500ms
      ASSERT_TRUE(p->cooldown >= 20 && p->cooldown <= 500);

      // hwtarget: 0.05f - 0.9f
      ASSERT_TRUE(p->hwtarget >= 0.05f && p->hwtarget <= 0.9f);

      // musicthresh: 0.0f - 1.0f
      ASSERT_TRUE(p->musicthresh >= 0.0f && p->musicthresh <= 1.0f);
    }
  }
}

void testDefaultPresetValues() {
  TEST_CASE("Default Preset - Expected Values");

  const PresetParams* p = PresetManager::getPresetParams(PresetId::DEFAULT);
  ASSERT_TRUE(p != nullptr);

  // Verify default preset has production values
  ASSERT_NEAR(p->hitthresh, 2.0f, 0.01f);
  ASSERT_NEAR(p->attackmult, 1.2f, 0.01f);
  ASSERT_NEAR(p->avgtau, 0.8f, 0.01f);
  ASSERT_EQUAL(30, p->cooldown);
  ASSERT_FALSE(p->adaptiveThresholdEnabled);
}

void testQuietPresetValues() {
  TEST_CASE("Quiet Preset - Lower Thresholds");

  const PresetParams* quiet = PresetManager::getPresetParams(PresetId::QUIET);
  const PresetParams* def = PresetManager::getPresetParams(PresetId::DEFAULT);

  ASSERT_TRUE(quiet != nullptr);
  ASSERT_TRUE(def != nullptr);

  // Quiet preset should have lower hit threshold for better detection
  ASSERT_TRUE(quiet->hitthresh < def->hitthresh);

  // Quiet preset should have adaptive threshold enabled
  ASSERT_TRUE(quiet->adaptiveThresholdEnabled);

  // Quiet preset should have lower music threshold for easier activation
  ASSERT_TRUE(quiet->musicthresh < def->musicthresh);
}

void testLoudPresetValues() {
  TEST_CASE("Loud Preset - Higher Thresholds");

  const PresetParams* loud = PresetManager::getPresetParams(PresetId::LOUD);
  const PresetParams* def = PresetManager::getPresetParams(PresetId::DEFAULT);

  ASSERT_TRUE(loud != nullptr);
  ASSERT_TRUE(def != nullptr);

  // Loud preset should have higher hit threshold to reject noise
  ASSERT_TRUE(loud->hitthresh > def->hitthresh);

  // Loud preset should have fast AGC disabled
  ASSERT_FALSE(loud->fastAgcEnabled);

  // Loud preset should have lower hwtarget for headroom
  ASSERT_TRUE(loud->hwtarget < def->hwtarget);
}

// Note: Full integration test for applyPreset() requires AdaptiveMic and MusicMode
// instances which depend on hardware (IPdmMic, ISystemTime). See integration tests
// in tests/integration/ for hardware-dependent testing.
//
// The tests below verify:
// 1. All preset parameters are within valid ranges (testPresetParamsRanges)
// 2. Preset-specific values match expected characteristics (testQuietPresetValues, etc.)
// 3. Return value behavior is correct (implicit in parsePresetName tests)

void runPresetTests() {
  Serial.println("=== PRESET MANAGER TESTS ===");

  // Name parsing tests
  testPresetNameParsing();
  testPresetNameParsingQuiet();
  testPresetNameParsingLoud();
  testPresetNameParsingLive();
  testPresetNameParsingCaseInsensitive();
  testPresetNameParsingMixedCase();
  testPresetNameParsingInvalid();
  testPresetNameParsingEmpty();
  testPresetNameParsingNull();

  // Name retrieval tests
  testPresetNameRetrieval();
  testPresetNameRetrievalQuiet();
  testPresetNameRetrievalLoud();
  testPresetNameRetrievalLive();
  testPresetNameRetrievalInvalid();

  // Preset system tests
  testPresetCount();
  testPresetParamsExist();
  testPresetParamsInvalidId();
  testPresetParamsRanges();

  // Specific preset value tests
  testDefaultPresetValues();
  testQuietPresetValues();
  testLoudPresetValues();

  Serial.println();
}
