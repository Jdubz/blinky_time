#include "BatteryMonitor.h"

bool BatteryMonitor::begin() {
  Config cfg;             // default-constructed config
  return begin(cfg);      // delegate to explicit overload
}

bool BatteryMonitor::begin(const Config& cfg) {
  cfg_ = cfg;

  // ADC setup
  #if defined(analogReadResolution)
  analogReadResolution(cfg_.adcBits);
  #endif

  #if defined(AR_INTERNAL2V4)
  if (cfg_.useInternal2V4Ref) {
    analogReference(AR_INTERNAL2V4);
  }
  #endif

  // Divider control
  if (cfg_.pinVBATEnable >= 0) {
    pinMode(cfg_.pinVBATEnable, OUTPUT);
    // Keep disabled until read (HIGH = disable on XIAO)
    digitalWrite(cfg_.pinVBATEnable, HIGH);
  }

  // HICHG control
  if (cfg_.pinHiChg >= 0) {
    pinMode(cfg_.pinHiChg, OUTPUT);
    // Default to "slow" 50 mA to be gentle
    digitalWrite(cfg_.pinHiChg, cfg_.hichgActiveLow ? HIGH : LOW);
  }

  // CHG status input
  if (cfg_.pinChgStatus >= 0) {
    pinMode(cfg_.pinChgStatus, INPUT_PULLUP);
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
  digitalWrite(cfg_.pinVBATEnable, on ? LOW : HIGH);
}

uint16_t BatteryMonitor::readOnceRaw_() {
  uint32_t acc = 0;
  uint8_t n = cfg_.samples > 0 ? cfg_.samples : 1;
  for (uint8_t i = 0; i < n; ++i) {
    acc += analogRead(cfg_.pinVBAT);
  }
  return (uint16_t)(acc / n);
}

uint16_t BatteryMonitor::readRaw() {
  enableDivider_(true);
  delay(3); // settle the MOSFET/divider & ADC mux
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
  bool out = enable ? (cfg_.hichgActiveLow ? LOW : HIGH)
                    : (cfg_.hichgActiveLow ? HIGH : LOW);
  digitalWrite(cfg_.pinHiChg, out);
}

bool BatteryMonitor::isCharging() const {
  if (cfg_.pinChgStatus < 0) return false;
  int v = digitalRead(cfg_.pinChgStatus);
  bool active = cfg_.chgActiveLow ? (v == LOW) : (v == HIGH);
  return active;
}

// Rough LiPo open-circuit voltage curve (no load)
// 4.20V -> 100% ; 3.70V -> ~50% ; 3.30V -> 0%
uint8_t BatteryMonitor::voltageToPercent(float v) {
  if (v <= 3.30f) return 0;
  if (v >= 4.20f) return 100;

  // Piecewise linear approximation for a pleasant gauge
  if (v < 3.50f) {
    // 3.30 -> 3.50 : 0% -> 10%
    return (uint8_t)((v - 3.30f) * (10.0f / 0.20f) + 0.5f);
  } else if (v < 3.70f) {
    // 3.50 -> 3.70 : 10% -> 40%
    return (uint8_t)(10 + (v - 3.50f) * (30.0f / 0.20f) + 0.5f);
  } else if (v < 3.90f) {
    // 3.70 -> 3.90 : 40% -> 75%
    return (uint8_t)(40 + (v - 3.70f) * (35.0f / 0.20f) + 0.5f);
  } else if (v < 4.05f) {
    // 3.90 -> 4.05 : 75% -> 92%
    return (uint8_t)(75 + (v - 3.90f) * (17.0f / 0.15f) + 0.5f);
  } else {
    // 4.05 -> 4.20 : 92% -> 100%
    return (uint8_t)(92 + (v - 4.05f) * (8.0f / 0.15f) + 0.5f);
  }
}
