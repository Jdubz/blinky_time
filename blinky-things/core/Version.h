#pragma once

/**
 * Blinky Time Version Information
 * Auto-generated from VERSION file - DO NOT EDIT MANUALLY
 * 
 * Follows Arduino Library Specification 1.5+ and Semantic Versioning 2.0.0
 * See: https://arduino.github.io/arduino-cli/latest/library-specification/
 */

// === Semantic Version Components ===
#define BLINKY_VERSION_MAJOR 1
#define BLINKY_VERSION_MINOR 0
#define BLINKY_VERSION_PATCH 1

// === Arduino Standard Version Macros ===
// Numerical version for comparison (Arduino standard)
// Format: MAJOR * 10000 + MINOR * 100 + PATCH
#define BLINKY_VERSION_NUMBER (BLINKY_VERSION_MAJOR * 10000 + BLINKY_VERSION_MINOR * 100 + BLINKY_VERSION_PATCH)

// String version (Arduino library.properties format)
#define BLINKY_VERSION_STRING "1.0.1"

// === Build Information ===
#define BLINKY_BUILD_DATE __DATE__
#define BLINKY_BUILD_TIME __TIME__
#define BLINKY_BUILD_TIMESTAMP __DATE__ " " __TIME__

// === Git Information ===
#define BLINKY_GIT_BRANCH "staging"
#define BLINKY_GIT_COMMIT "96e6954"

// === Version Display Strings ===
#define BLINKY_FULL_VERSION "Blinky Time v" BLINKY_VERSION_STRING " (" BLINKY_GIT_BRANCH ")"
#define BLINKY_VERSION_WITH_BUILD BLINKY_VERSION_STRING " [" BLINKY_GIT_COMMIT "] " BLINKY_BUILD_DATE

// === Arduino Library Compatibility ===
// Standard Arduino version check macros (like ESP32, WiFi libraries)
#define BLINKY_VERSION_CHECK(major, minor, patch) (BLINKY_VERSION_NUMBER >= ((major) * 10000 + (minor) * 100 + (patch)))

// === Pre-release and Development Versions ===
// Uncomment for development builds
// #define BLINKY_VERSION_PRERELEASE "alpha.1"
// #define BLINKY_VERSION_DEV_BUILD

// === Runtime Version Functions ===
#ifdef __cplusplus
namespace BlinkyVersion {
    // Version comparison functions (like semver in JavaScript)
    inline bool isGreaterThan(uint8_t major, uint8_t minor, uint8_t patch) {
        return BLINKY_VERSION_CHECK(major, minor, patch + 1);
    }
    
    inline bool isAtLeast(uint8_t major, uint8_t minor, uint8_t patch) {
        return BLINKY_VERSION_CHECK(major, minor, patch);
    }
    
    // Get version components at runtime
    constexpr uint8_t getMajor() { return BLINKY_VERSION_MAJOR; }
    constexpr uint8_t getMinor() { return BLINKY_VERSION_MINOR; }
    constexpr uint8_t getPatch() { return BLINKY_VERSION_PATCH; }
    constexpr uint32_t getNumber() { return BLINKY_VERSION_NUMBER; }
    
    // Version string getters
    constexpr const char* getString() { return BLINKY_VERSION_STRING; }
    constexpr const char* getFullVersion() { return BLINKY_FULL_VERSION; }
    constexpr const char* getBuildInfo() { return BLINKY_VERSION_WITH_BUILD; }
    constexpr const char* getGitCommit() { return BLINKY_GIT_COMMIT; }
    constexpr const char* getGitBranch() { return BLINKY_GIT_BRANCH; }
}
#endif