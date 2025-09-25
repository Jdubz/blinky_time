/**
 * Arduino Standard Versioning Test
 * 
 * This file demonstrates how to use the Arduino-standard versioning system
 * that's now implemented in Blinky Time.
 */

#include "core/Version.h"

void setup() {
    Serial.begin(115200);
    while (!Serial) { delay(10); }
    
    // === Basic Version Information ===
    Serial.println("=== ARDUINO VERSIONING DEMO ===");
    Serial.println();
    
    // Display version using Arduino-standard macros
    Serial.print("Version Number: ");
    Serial.println(BLINKY_VERSION_NUMBER);  // 10001
    
    Serial.print("Version String: ");
    Serial.println(BLINKY_VERSION_STRING);  // "1.0.1"
    
    Serial.print("Full Version: ");
    Serial.println(BLINKY_FULL_VERSION);    // "Blinky Time v1.0.1 (staging)"
    
    // === Arduino Library Standard Version Checks ===
    Serial.println();
    Serial.println("=== VERSION COMPATIBILITY CHECKS ===");
    
    // Arduino-style compile-time version checks (like ESP32 library)
    #if BLINKY_VERSION_CHECK(1, 0, 0)
        Serial.println("✅ Version >= 1.0.0 (compile-time check)");
    #else
        Serial.println("❌ Version < 1.0.0");
    #endif
    
    #if BLINKY_VERSION_CHECK(1, 1, 0)
        Serial.println("✅ Version >= 1.1.0 - New features available");
    #else
        Serial.println("❌ Version < 1.1.0 - Legacy mode");
    #endif
    
    #if BLINKY_VERSION_CHECK(2, 0, 0)
        Serial.println("✅ Version >= 2.0.0 - Breaking changes");
    #else
        Serial.println("❌ Version < 2.0.0 - Stable API");
    #endif
    
    // === Runtime Version Checks (like JavaScript semver) ===
    Serial.println();
    Serial.println("=== RUNTIME VERSION FUNCTIONS ===");
    
    // JavaScript-style runtime checks
    if (BlinkyVersion::isAtLeast(1, 0, 0)) {
        Serial.println("✅ Runtime check: >= 1.0.0");
    }
    
    if (BlinkyVersion::isGreaterThan(0, 9, 9)) {
        Serial.println("✅ Runtime check: > 0.9.9");
    }
    
    if (!BlinkyVersion::isAtLeast(2, 0, 0)) {
        Serial.println("❌ Runtime check: < 2.0.0");
    }
    
    // === Component Access ===
    Serial.println();
    Serial.println("=== VERSION COMPONENTS ===");
    
    Serial.print("Major: ");
    Serial.println(BlinkyVersion::getMajor());      // 1
    
    Serial.print("Minor: ");
    Serial.println(BlinkyVersion::getMinor());      // 0
    
    Serial.print("Patch: ");
    Serial.println(BlinkyVersion::getPatch());      // 1
    
    Serial.print("Number: ");
    Serial.println(BlinkyVersion::getNumber());     // 10001
    
    // === Build Information ===
    Serial.println();
    Serial.println("=== BUILD INFORMATION ===");
    
    Serial.print("Build Date: ");
    Serial.println(BLINKY_BUILD_DATE);
    
    Serial.print("Build Time: ");
    Serial.println(BLINKY_BUILD_TIME);
    
    Serial.print("Git Branch: ");
    Serial.println(BlinkyVersion::getGitBranch());  // "staging"
    
    Serial.print("Git Commit: ");
    Serial.println(BlinkyVersion::getGitCommit());  // "89e0c47"
    
    // === Version-Dependent Code Examples ===
    Serial.println();
    Serial.println("=== VERSION-DEPENDENT FEATURES ===");
    
    // Example: Enable features based on version
    Serial.print("Feature Set: ");
    #if BLINKY_VERSION_CHECK(1, 1, 0)
        Serial.println("Advanced Features Enabled");
    #elif BLINKY_VERSION_CHECK(1, 0, 0)
        Serial.println("Basic Features Only");
    #else
        Serial.println("Legacy Mode");
    #endif
    
    // Runtime feature detection
    if (BlinkyVersion::isAtLeast(1, 0, 1)) {
        Serial.println("✅ Bug fixes from 1.0.1 available");
    }
    
    Serial.println();
    Serial.println("=== COMPARISON WITH JAVASCRIPT/NPM ===");
    Serial.println("Arduino:         BlinkyVersion::isAtLeast(1,0,0)");
    Serial.println("JavaScript:      semver.gte('1.0.0')");
    Serial.println();
    Serial.println("Arduino:         BLINKY_VERSION_CHECK(1,0,0)");
    Serial.println("JavaScript:      process.env.npm_package_version");
    Serial.println();
    Serial.println("Both follow semantic versioning: MAJOR.MINOR.PATCH");
}

void loop() {
    // Nothing to do in loop for this demo
    delay(10000);
}