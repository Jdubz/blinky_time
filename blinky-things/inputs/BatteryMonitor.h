#pragma once
#include <stdint.h>
#include "../hal/interfaces/IGpio.h"
#include "../hal/interfaces/IAdc.h"
#include "../hal/interfaces/ISystemTime.h"
#include "../hal/PlatformConstants.h"

// Default pins for XIAO BLE / Sense (override via Config if your core differs)
#ifndef PIN_VBAT
  #if defined(P0_31)
    #define PIN_VBAT        P0_31    // ADC input for VBAT divider (mbed core)
  #else
    #define PIN_VBAT        31       // ADC input for VBAT divider (non-mbed core)
  #endif
#endif
#ifndef VBAT_ENABLE_PIN
  #if defined(P0_14)
    #define VBAT_ENABLE_PIN P0_14    // LOW = enable divider to ADC, HIGH = disable (mbed)
  #else
    #define VBAT_ENABLE_PIN 14       // LOW = enable divider to ADC, HIGH = disable (non-mbed)
  #endif
#endif

// HICHG (fast-charge) control:
//  - mbed core typically exposes P0_13 (LOW = 100mA, HIGH = 50mA)
//  - non-mbed is often "22" for the same line
#ifndef HICHG_PIN_DEFAULT
  #if defined(P0_13)
    #define HICHG_PIN_DEFAULT P0_13
  #else
    #define HICHG_PIN_DEFAULT 22
  #endif
#endif

// CHG status pin: many cores wire it to P0_17 (active LOW while charging)
#ifndef CHG_STATUS_PIN_DEFAULT
  #if defined(P0_17)
    #define CHG_STATUS_PIN_DEFAULT P0_17  // (mbed core)
  #else
    #define CHG_STATUS_PIN_DEFAULT 17     // (non-mbed core)
  #endif
#endif

/**
 * BatteryMonitor - Monitors battery voltage and charging status
 *
 * Uses HAL interfaces for hardware abstraction, enabling unit testing.
 * All battery voltage thresholds are centralized in PlatformConstants.h.
 */
class BatteryMonitor {
public:
  struct Config {
    // Hardware pins
    int pinVBAT        = PIN_VBAT;
    int pinVBATEnable  = VBAT_ENABLE_PIN;   // set <0 to disable switching (always on)
    int pinHiChg       = HICHG_PIN_DEFAULT; // set <0 to disable fast-charge control
    int pinChgStatus   = CHG_STATUS_PIN_DEFAULT; // set <0 if not available

    // Behavior (defaults from PlatformConstants)
    bool hichgActiveLow   = Platform::Charging::HICHG_ACTIVE_LOW;
    bool chgActiveLow     = Platform::Charging::CHG_ACTIVE_LOW;
    bool useInternal2V4Ref= true;   // use AR_INTERNAL2V4 if available
    uint8_t adcBits       = Platform::Adc::DEFAULT_RESOLUTION;
    uint8_t samples       = Platform::Adc::DEFAULT_SAMPLES;
    float dividerRatio    = Platform::Battery::DIVIDER_RATIO;

    // Reference voltage when using AR_INTERNAL2V4
    float vrefVolts       = Platform::Battery::VREF_2V4;

    // Simple low-pass for update()
    float lpAlpha         = 0.25f;  // 0..1 (higher = quicker)
  };

  /**
   * Construct with HAL dependencies for testability
   */
  BatteryMonitor(IGpio& gpio, IAdc& adc, ISystemTime& time);

  bool begin();    // uses a default Config internally
  bool begin(const Config& cfg);      // explicit config

  // One-shot read (enables divider, samples, disables)
  float readVoltage();      // volts
  uint16_t readRaw();       // raw ADC units (0..2^adcBits-1)

  // Periodic smoother (calls readVoltage() internally)
  void update();
  float getVoltage() const { return lastVoltage_; }     // smoothed volts
  uint8_t getPercent() const { return lastPercent_; }   // 0..100 (approximate)

  // Charger helpers (optional)
  void setFastCharge(bool enable); // controls HICHG if configured
  bool isCharging() const;         // true if CHG pin present & active

  // Utility: LiPo OCV → % (no load, rough curve)
  static uint8_t voltageToPercent(float v);

private:
  // HAL references
  IGpio& gpio_;
  IAdc& adc_;
  ISystemTime& time_;

  Config cfg_;
  bool inited_ = false;
  float lastVoltage_ = 0.0f;
  uint8_t lastPercent_ = 0;

  // Helpers
  void enableDivider_(bool on);
  uint16_t readOnceRaw_();
};
