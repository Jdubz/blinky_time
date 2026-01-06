#pragma once
#include "../render/LEDMapper.h"
#include "../devices/DeviceConfig.h"

// Global LED mapper instance
extern LEDMapper ledMapper;
extern DeviceConfig config;  // v28: Changed to non-const for runtime loading
