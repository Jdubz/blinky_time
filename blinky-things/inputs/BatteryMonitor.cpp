#include "BatteryMonitor.h"

BatteryMonitor::BatteryMonitor(IGpio& gpio, IAdc& adc, ISystemTime& time)
    : gpio_(gpio), adc_(adc), time_(time) {
}

bool BatteryMonitor::begin() {
  Config cfg;             // default-constructed config
  return begin(cfg);      // delegate to explicit overload
}

bool BatteryMonitor::begin(const Config& cfg) {
  cfg_ = cfg;

  // ADC setup
  adc_.setResolution(cfg_.adcBits);

  if (cfg_.useInternal2V4Ref) {
    adc_.setReference(IAdc::REF_INTERNAL_2V4);
  }

  // Divider control
  if (cfg_.pinVBATEnable >= 0) {
    gpio_.pinMode(cfg_.pinVBATEnable, IGpio::OUTPUT_MODE);
    // Keep disabled until read (HIGH = disable on XIAO)
    gpio_.digitalWrite(cfg_.pinVBATEnable, IGpio::HIGH_LEVEL);
  }

  // HICHG control
  if (cfg_.pinHiChg >= 0) {
    gpio_.pinMode(cfg_.pinHiChg, IGpio::OUTPUT_MODE);
    // Default to "slow" 50 mA to be gentle
    gpio_.digitalWrite(cfg_.pinHiChg, cfg_.hichgActiveLow ? IGpio::HIGH_LEVEL : IGpio::LOW_LEVEL);
  }

  // CHG status input
  if (cfg_.pinChgStatus >= 0) {
    gpio_.pinMode(cfg_.pinChgStatus, IGpio::INPUT_PULLUP_MODE);
  }

  // Seed smoothed value
  lastVoltage_ = readVoltage();
  lastPercent_ = voltageToPercent(lastVoltage_);

  inited_ = true;
  return true;
}

void BatteryMonitor::enableDivider_(bool on) {
  if (cfg_.pinVBATEnable < 0) return;
  // On XIAO BLE: LOW = enable divider, HIGH = disable
  gpio_.digitalWrite(cfg_.pinVBATEnable, on ? IGpio::LOW_LEVEL : IGpio::HIGH_LEVEL);
}

uint16_t BatteryMonitor::readOnceRaw_() {
  uint32_t acc = 0;
  uint8_t n = cfg_.samples > 0 ? cfg_.samples : 1;
  for (uint8_t i = 0; i < n; ++i) {
    acc += adc_.analogRead(cfg_.pinVBAT);
  }
  return (uint16_t)(acc / n);
}

uint16_t BatteryMonitor::readRaw() {
  enableDivider_(true);
  time_.delay(20); // settle the MOSFET/divider & ADC mux (increased from 3ms)
  uint16_t raw = readOnceRaw_();
  enableDivider_(false);
  return raw;
}

float BatteryMonitor::readVoltage() {
  // Read raw
  uint16_t raw = readRaw();

  // Convert raw to volts at ADC pin
  const float maxCount = (cfg_.adcBits == 12) ? 4095.0f : ((cfg_.adcBits == 10) ? 1023.0f : (float)((1UL<<cfg_.adcBits)-1));
  float vref = cfg_.vrefVolts;

  // If the core exposes analogReadMilliVolts, you could swap this calculation
  float v_adc = (raw * vref) / maxCount;

  // Undo divider to get battery voltage
  float v_batt = v_adc / cfg_.dividerRatio;

  // Sanity check: LiPo batteries should be between 2.5V and 4.3V
  // Readings outside this range indicate hardware/configuration issues
  if (v_batt < 2.0f || v_batt > 5.0f) {
    // Invalid reading - return last known good value if available
    if (lastVoltage_ >= 2.0f && lastVoltage_ <= 5.0f) {
      return lastVoltage_;
    }
    // No good value available, return a clearly invalid value
    return 0.0f;
  }

  return v_batt;
}

void BatteryMonitor::update() {
  float v = readVoltage();
  // Low-pass filter
  lastVoltage_ = (1.0f - cfg_.lpAlpha) * lastVoltage_ + cfg_.lpAlpha * v;
  lastPercent_ = voltageToPercent(lastVoltage_);
}

void BatteryMonitor::setFastCharge(bool enable) {
  if (cfg_.pinHiChg < 0) return;
  uint8_t out = enable ? (cfg_.hichgActiveLow ? IGpio::LOW_LEVEL : IGpio::HIGH_LEVEL)
                       : (cfg_.hichgActiveLow ? IGpio::HIGH_LEVEL : IGpio::LOW_LEVEL);
  gpio_.digitalWrite(cfg_.pinHiChg, out);
}

bool BatteryMonitor::isBatteryConnected() const {
  // Battery is considered connected if voltage is in valid LiPo range
  // 2.5V - 4.3V is the reasonable operating range for LiPo batteries
  float v = lastVoltage_;
  return (v >= 2.5f && v <= 4.3f);
}

bool BatteryMonitor::isCharging() const {
  // Can't be charging without a battery connected
  if (!isBatteryConnected()) return false;

  if (cfg_.pinChgStatus < 0) return false;
  int v = gpio_.digitalRead(cfg_.pinChgStatus);
  bool active = cfg_.chgActiveLow ? (v == IGpio::LOW_LEVEL) : (v == IGpio::HIGH_LEVEL);
  return active;
}


// Rough LiPo open-circuit voltage curve (no load)
// Uses Platform::Battery constants for thresholds
uint8_t BatteryMonitor::voltageToPercent(float v) {
  constexpr float V_EMPTY = Platform::Battery::VOLTAGE_CRITICAL;  // 3.30V -> 0%
  constexpr float V_FULL  = Platform::Battery::VOLTAGE_FULL;      // 4.20V -> 100%
  constexpr float V_LOW   = Platform::Battery::VOLTAGE_LOW;       // 3.50V -> ~10%
  constexpr float V_NOM   = Platform::Battery::VOLTAGE_NOMINAL;   // 3.70V -> ~40%

  if (v <= V_EMPTY) return 0;
  if (v >= V_FULL) return 100;

  // Piecewise linear approximation for a pleasant gauge
  if (v < V_LOW) {
    // 3.30 -> 3.50 : 0% -> 10%
    return (uint8_t)((v - V_EMPTY) * (10.0f / (V_LOW - V_EMPTY)) + 0.5f);
  } else if (v < V_NOM) {
    // 3.50 -> 3.70 : 10% -> 40%
    return (uint8_t)(10 + (v - V_LOW) * (30.0f / (V_NOM - V_LOW)) + 0.5f);
  } else if (v < 3.90f) {
    // 3.70 -> 3.90 : 40% -> 75%
    return (uint8_t)(40 + (v - V_NOM) * (35.0f / 0.20f) + 0.5f);
  } else if (v < 4.05f) {
    // 3.90 -> 4.05 : 75% -> 92%
    return (uint8_t)(75 + (v - 3.90f) * (17.0f / 0.15f) + 0.5f);
  } else {
    // 4.05 -> 4.20 : 92% -> 100%
    return (uint8_t)(92 + (v - 4.05f) * (8.0f / (V_FULL - 4.05f)) + 0.5f);
  }
}

void BatteryMonitor::testDividerEnable() {
  if (cfg_.pinVBATEnable < 0) {
    Serial.println(F("ERROR: No enable pin configured"));
    return;
  }

  Serial.println(F("=== Battery Divider Enable Test ==="));
  Serial.print(F("PIN_VBAT_ENABLE: "));
  Serial.println(cfg_.pinVBATEnable);

  // Test 1: Divider DISABLED (HIGH)
  Serial.println(F("\nTest 1: Divider DISABLED (pin HIGH)"));
  gpio_.digitalWrite(cfg_.pinVBATEnable, IGpio::HIGH_LEVEL);
  time_.delay(50);
  uint16_t rawHigh = readOnceRaw_();
  Serial.print(F("  Raw ADC: "));
  Serial.println(rawHigh);

  // Test 2: Divider ENABLED (LOW)
  Serial.println(F("\nTest 2: Divider ENABLED (pin LOW)"));
  gpio_.digitalWrite(cfg_.pinVBATEnable, IGpio::LOW_LEVEL);
  time_.delay(50);
  uint16_t rawLow = readOnceRaw_();
  Serial.print(F("  Raw ADC: "));
  Serial.println(rawLow);

  // Return to disabled state
  gpio_.digitalWrite(cfg_.pinVBATEnable, IGpio::HIGH_LEVEL);

  // Analyze results
  Serial.println(F("\n=== Results ==="));
  if (rawHigh == rawLow) {
    Serial.println(F("ERROR: No change! Enable pin not working!"));
    Serial.println(F("Possible issues:"));
    Serial.println(F("  - Wrong pin number for this platform"));
    Serial.println(F("  - Pin not connected to divider circuit"));
    Serial.println(F("  - Hardware issue with MOSFET"));
  } else if (rawHigh < rawLow) {
    Serial.println(F("ERROR: Logic is INVERTED!"));
    Serial.println(F("  HIGH = enabled (should be disabled)"));
    Serial.println(F("  LOW = disabled (should be enabled)"));
  } else {
    Serial.println(F("SUCCESS: Enable pin working correctly!"));
    Serial.print(F("  Difference: "));
    Serial.print(rawHigh - rawLow);
    Serial.println(F(" counts"));
  }
  Serial.println(F("==========================="));
}

void BatteryMonitor::scanForEnablePin() {
  Serial.println(F("=== Scanning for Correct Enable Pin ==="));
  Serial.println(F("Testing pins 0-31 to find voltage divider control..."));
  Serial.println();

  // Get baseline reading with current pin disabled
  if (cfg_.pinVBATEnable >= 0) {
    gpio_.digitalWrite(cfg_.pinVBATEnable, IGpio::HIGH_LEVEL);
    time_.delay(50);
  }
  uint16_t baseline = readOnceRaw_();
  Serial.print(F("Baseline (no divider): "));
  Serial.println(baseline);
  Serial.println();

  // Try each pin
  for (int pin = 0; pin <= 31; pin++) {
    // Skip the VBAT pin itself
    if (pin == cfg_.pinVBAT) continue;

    // Set pin as output
    gpio_.pinMode(pin, IGpio::OUTPUT_MODE);

    // Test LOW (should enable divider if correct pin)
    gpio_.digitalWrite(pin, IGpio::LOW_LEVEL);
    time_.delay(50);
    uint16_t adcLow = readOnceRaw_();

    // Test HIGH (should disable divider)
    gpio_.digitalWrite(pin, IGpio::HIGH_LEVEL);
    time_.delay(50);
    uint16_t adcHigh = readOnceRaw_();

    // Check if this pin affects the reading
    int diff = abs((int)adcHigh - (int)adcLow);
    if (diff > 50) {  // Significant change
      Serial.print(F("*** FOUND IT! Pin "));
      Serial.print(pin);
      Serial.print(F(" changes ADC by "));
      Serial.print(diff);
      Serial.println(F(" counts"));
      Serial.print(F("  LOW="));
      Serial.print(adcLow);
      Serial.print(F(", HIGH="));
      Serial.println(adcHigh);

      if (adcHigh > adcLow) {
        Serial.println(F("  Logic: LOW=enabled, HIGH=disabled (correct)"));
      } else {
        Serial.println(F("  Logic: HIGH=enabled, LOW=disabled (inverted!)"));
      }
    }
  }

  Serial.println();
  Serial.println(F("Scan complete. Update PIN_VBAT_ENABLE in code."));
  Serial.println(F("==========================="));
}
