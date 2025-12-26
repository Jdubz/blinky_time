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
  time_.delay(Platform::Battery::ADC_SETTLE_TIME_MS); // settle the MOSFET/divider & ADC mux
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

  // Sanity check: Readings outside the physically plausible range
  // indicate hardware/configuration issues
  if (v_batt < Platform::Battery::MIN_VALID_VOLTAGE || v_batt > Platform::Battery::MAX_VALID_VOLTAGE) {
    // Invalid reading - return last known good value if available
    if (lastVoltage_ >= Platform::Battery::MIN_VALID_VOLTAGE && lastVoltage_ <= Platform::Battery::MAX_VALID_VOLTAGE) {
      return lastVoltage_;
    }
    // No good value available, return a clearly invalid value
    return 0.0f;
  }

  return v_batt;
}

void BatteryMonitor::update(float dt) {
  float v = readVoltage();

  // FIX: Use time-based smoothing when dt is provided (frame-rate independent)
  float alpha;
  if (dt > 0.0f) {
    // Time-based: convert lpAlpha to time constant and use exponential smoothing
    // Assume lpAlpha was intended for ~30ms updates (typical battery check rate)
    constexpr float NOMINAL_UPDATE_RATE = 0.03f;  // 30ms
    float tau = -NOMINAL_UPDATE_RATE / logf(1.0f - cfg_.lpAlpha);
    alpha = 1.0f - expf(-dt / tau);
  } else {
    // Backwards compatibility: use lpAlpha directly if dt not provided
    alpha = cfg_.lpAlpha;
  }

  // Low-pass filter with calculated alpha
  lastVoltage_ = (1.0f - alpha) * lastVoltage_ + alpha * v;
  lastPercent_ = voltageToPercent(lastVoltage_);
}

void BatteryMonitor::setFastCharge(bool enable) {
  if (cfg_.pinHiChg < 0) return;
  uint8_t out = enable ? (cfg_.hichgActiveLow ? IGpio::LOW_LEVEL : IGpio::HIGH_LEVEL)
                       : (cfg_.hichgActiveLow ? IGpio::HIGH_LEVEL : IGpio::LOW_LEVEL);
  gpio_.digitalWrite(cfg_.pinHiChg, out);
}

bool BatteryMonitor::isBatteryConnected() const {
  // Battery is considered connected if voltage is in valid LiPo operating range
  float v = lastVoltage_;
  return (v >= Platform::Battery::MIN_CONNECTED_VOLTAGE && v <= Platform::Battery::MAX_CONNECTED_VOLTAGE);
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

