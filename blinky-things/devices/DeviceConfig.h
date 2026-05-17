#pragma once
#include <stdint.h>
#include "../hal/PlatformConstants.h"

enum MatrixOrientation {
  HORIZONTAL = 0,         // Standard horizontal row-major (fire-totem, bucket-totem)
  VERTICAL = 1,           // Vertical column-major zigzag (tube-light)
  PANEL_GRID = 2,         // 2×2 grid of equal panels, chained TL→TR→BL→BR, serpentine rows
  HORIZONTAL_ZIGZAG = 3   // Row-major serpentine — data starts top-left, row 0 L→R,
                          // row 1 R→L, row 2 L→R, ... (big-bucket-style wiring)
};

enum LayoutType {
  MATRIX_LAYOUT = 0,  // 2D matrix arrangement with upward heat propagation
  LINEAR_LAYOUT = 1,  // Linear/string arrangement with lateral heat propagation
  RANDOM_LAYOUT = 2   // Random/scattered arrangement with omnidirectional heat propagation
};

struct MatrixConfig {
  uint8_t width;
  uint8_t height;
  uint8_t ledPin;
  // Second LED data pin for two-strand devices. 0 = single strand (default).
  // When non-zero, pixels [0..total/2) go to `ledPin`, pixels [total/2..total)
  // go to `ledPin2`; the renderer is unaware of the split (CompositeLedStrip
  // dispatches the writes in hardware/).
  uint8_t ledPin2;
  uint8_t brightness;
  uint32_t ledType;
  MatrixOrientation orientation;
  LayoutType layoutType;
};

struct ChargingConfig {
  bool fastChargeEnabled;
  float lowBatteryThreshold;
  float criticalBatteryThreshold;
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

struct BlinkySerialConfig {
  uint32_t baudRate;
  uint16_t initTimeoutMs;
};

struct MicConfig {
  uint16_t sampleRate;
  uint8_t bufferSize;
};

struct InputConfig {
  // GPIO pin for the "cycle to next generator" button. 0 = unused (no button).
  // Pin is read with INPUT_PULLUP (button shorts to GND on press). The polling
  // + debounce logic lives in the main loop; this struct just carries the
  // wiring info from the per-device config.
  uint8_t buttonPin;
};

struct DeviceConfig {
  const char* deviceName;
  MatrixConfig matrix;
  ChargingConfig charging;
  IMUConfig imu;
  BlinkySerialConfig serial;
  MicConfig microphone;
  InputConfig input;
};
