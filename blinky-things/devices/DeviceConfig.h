#pragma once
#include <stdint.h>

enum MatrixOrientation {
  HORIZONTAL = 0,  // Standard horizontal layout (fire-totem)
  VERTICAL = 1     // Vertical layout (tube-light)
};

enum FireEffectType {
  MATRIX_FIRE = 0,  // Traditional upward-propagating fire for matrices
  STRING_FIRE = 1   // Sideways-dissipating fire for LED strings
};

struct MatrixConfig {
  uint8_t width;
  uint8_t height;
  uint8_t ledPin;
  uint8_t brightness;
  uint32_t ledType;
  MatrixOrientation orientation;
  FireEffectType fireType;
};

struct ChargingConfig {
  bool fastChargeEnabled;
  float lowBatteryThreshold;
  float criticalBatteryThreshold;
  bool autoShowVisualizationWhenCharging;
  float minVoltage;
  float maxVoltage;
};

struct IMUConfig {
  float upVectorX;        // Default up vector X (reserved for future use)
  float upVectorY;        // Default up vector Y (reserved for future use)
  float upVectorZ;        // Default up vector Z (reserved for future use)
  bool invertZ;           // Invert Z axis for mounting orientation
  float rotationDegrees;  // Rotation angle for cylindrical mounting
  bool swapXY;            // Swap X and Y axes
  bool invertX;           // Invert X axis
  bool invertY;           // Invert Y axis
};

struct SerialConfig {
  uint32_t baudRate;
  uint16_t initTimeoutMs;
};

struct MicConfig {
  uint16_t sampleRate;
  uint8_t bufferSize;
};

struct FireDefaults {
  uint8_t baseCooling;
  uint8_t sparkHeatMin;
  uint8_t sparkHeatMax;
  float sparkChance;
  float audioSparkBoost;
  uint8_t audioHeatBoostMax;
  int8_t coolingAudioBias;
  uint8_t bottomRowsForSparks;
  uint8_t transientHeatMax;
};

struct DeviceConfig {
  const char* deviceName;
  MatrixConfig matrix;
  ChargingConfig charging;
  IMUConfig imu;
  SerialConfig serial;
  MicConfig microphone;
  FireDefaults fireDefaults;
};
