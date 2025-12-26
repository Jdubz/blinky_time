#pragma once

/**
 * DebugLog.h - Compile-time debug level system
 *
 * Provides DEBUG_ERROR, DEBUG_WARN, DEBUG_INFO, and DEBUG_VERBOSE macros
 * that can be compiled out for production builds to save flash and improve
 * performance (Serial.println can take 1-10ms per call).
 *
 * Usage:
 *   #include "config/DebugLog.h"
 *
 *   DEBUG_ERROR(F("Critical error!"));
 *   DEBUG_WARN(F("Warning: Battery low"));
 *   DEBUG_INFO(F("Initialized successfully"));
 *   DEBUG_VERBOSE(F("Frame time: 16ms"));
 *
 * Debug Levels:
 *   0 = NONE    - All debug output disabled (production)
 *   1 = ERROR   - Only critical errors
 *   2 = WARN    - Errors + warnings
 *   3 = INFO    - Errors + warnings + general info (default)
 *   4 = VERBOSE - All debug output including verbose diagnostics
 *
 * Set DEBUG_LEVEL in platformio.ini or before including this file:
 *   build_flags = -DDEBUG_LEVEL=1  ; Production: errors only
 *   build_flags = -DDEBUG_LEVEL=3  ; Development: info + warnings + errors
 */

#ifndef DEBUG_LEVEL
#define DEBUG_LEVEL 3  // Default: INFO level (errors + warnings + info)
#endif

// Debug macros - compile out based on DEBUG_LEVEL
#if DEBUG_LEVEL >= 1
  #define DEBUG_ERROR(x) Serial.println(x)
  #define DEBUG_ERROR_F(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#else
  #define DEBUG_ERROR(x) ((void)0)
  #define DEBUG_ERROR_F(fmt, ...) ((void)0)
#endif

#if DEBUG_LEVEL >= 2
  #define DEBUG_WARN(x) Serial.println(x)
  #define DEBUG_WARN_F(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#else
  #define DEBUG_WARN(x) ((void)0)
  #define DEBUG_WARN_F(fmt, ...) ((void)0)
#endif

#if DEBUG_LEVEL >= 3
  #define DEBUG_INFO(x) Serial.println(x)
  #define DEBUG_INFO_F(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#else
  #define DEBUG_INFO(x) ((void)0)
  #define DEBUG_INFO_F(fmt, ...) ((void)0)
#endif

#if DEBUG_LEVEL >= 4
  #define DEBUG_VERBOSE(x) Serial.println(x)
  #define DEBUG_VERBOSE_F(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#else
  #define DEBUG_VERBOSE(x) ((void)0)
  #define DEBUG_VERBOSE_F(fmt, ...) ((void)0)
#endif

// Utility: Print with label
#if DEBUG_LEVEL >= 3
  #define DEBUG_PRINT_VAR(name, value) \
    do { \
      Serial.print(F(name ": ")); \
      Serial.println(value); \
    } while(0)
#else
  #define DEBUG_PRINT_VAR(name, value) ((void)0)
#endif
