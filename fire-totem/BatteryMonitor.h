#pragma once
#include <Arduino.h>

// Default pins for XIAO BLE / Sense (override via Config if your core differs)
#ifndef PIN_VBAT
  #define PIN_VBAT        P0_31    // ADC input for VBAT divider
#endif
#ifndef VBAT_ENABLE_PIN
  #define VBAT_ENABLE_PIN P0_14    // LOW = enable divider to ADC, HIGH = disable
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
  #define CHG_STATUS_PIN_DEFAULT P0_17
#endif

class BatteryMonitor {
public:
  struct Config {
    // Hardware pins
    int pinVBAT        = PIN_VBAT;
    int pinVBATEnable  = VBAT_ENABLE_PIN;   // set <0 to disable switching (always on)
    int pinHiChg       = HICHG_PIN_DEFAULT; // set <0 to disable fast-charge control
    int pinChgStatus   = CHG_STATUS_PIN_DEFAULT; // set <0 if not available

    // Behavior
    bool hichgActiveLow   = true;   // LOW => fast charge
    bool chgActiveLow     = true;   // LOW while charging
    bool useInternal2V4Ref= true;   // use AR_INTERNAL2V4 if available
    uint8_t adcBits       = 12;     // 10/12 depending on core
    uint8_t samples       = 8;      // oversampling
    float dividerRatio    = (1.0f/3.0f); // VBAT -> ADC ≈ VBAT/3

    // Reference voltage when using AR_INTERNAL2V4
    float vrefVolts       = 2.4f;

    // Simple low-pass for update()
    float lpAlpha         = 0.25f;  // 0..1 (higher = quicker)
  };

  bool begin(const Config& cfg = Config());

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
  Config cfg_;
  bool inited_ = false;
  float lastVoltage_ = 0.0f;
  uint8_t lastPercent_ = 0;

  // Helpers
  void enableDivider_(bool on);
  uint16_t readOnceRaw_();
};
