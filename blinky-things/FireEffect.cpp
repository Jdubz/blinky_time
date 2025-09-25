#include "FireEffect.h"
#include "SerialConsole.h"
#include "configs/DeviceConfig.h"


FireEffect::FireEffect(Adafruit_NeoPixel &strip, int width, int height)
    : leds(strip), WIDTH(width), HEIGHT(height), heat(nullptr) {
    restoreDefaults();
}

void FireEffect::begin() {
    if (heat) {
        free(heat);
        heat = nullptr;
    }

    heat = (float*)malloc(sizeof(float) * WIDTH * HEIGHT);
    if (!heat) {
        Serial.println(F("FireEffect: Failed to allocate heat buffer"));
        return;
    }

    for (int y = 0; y < HEIGHT; ++y)
        for (int x = 0; x < WIDTH;  ++x)
            getHeatRef(x,y) = 0.0f;
}

void FireEffect::restoreDefaults() {
    // Use device config defaults instead of global defaults for consistency
    params.baseCooling         = config.fireDefaults.baseCooling;
    params.sparkHeatMin        = config.fireDefaults.sparkHeatMin;
    params.sparkHeatMax        = config.fireDefaults.sparkHeatMax;
    params.sparkChance         = config.fireDefaults.sparkChance;
    params.audioSparkBoost     = config.fireDefaults.audioSparkBoost;
    params.audioHeatBoostMax   = config.fireDefaults.audioHeatBoostMax;
    params.coolingAudioBias    = config.fireDefaults.coolingAudioBias;
    params.bottomRowsForSparks = config.fireDefaults.bottomRowsForSparks;
    params.transientHeatMax    = config.fireDefaults.transientHeatMax;
}

void FireEffect::update(float energy, float hit) {
    // Balanced ember floor - allows quiet adaptation but reduces silence activity
    float emberFloor = 0.03f; // 3% energy floor (balanced between 1% and 5%)
    float boostedEnergy = max(emberFloor, energy * (1.0f + hit * (params.transientHeatMax / 255.0f)));

    // --- frame dt (seconds) ---
    unsigned long nowMs = millis();
    float dt = (lastUpdateMs == 0) ? 0.0f : (nowMs - lastUpdateMs) * 0.001f;
    lastUpdateMs = nowMs;

    // Cooling bias by audio (negative = taller flames for loud parts)
    int16_t coolingBias = params.coolingAudioBias; // int8_t promoted
    int cooling = params.baseCooling + coolingBias; // can go below 0; clamp in coolCells

    coolCells();

    propagateUp();

    injectSparks(boostedEnergy);


    render();
}


void FireEffect::coolCells() {
    const float coolingScale = 0.5f / 255.0f;
    const int maxCooling = params.baseCooling + 1;

    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            // Simple random cooling
            const float decay = random(0, maxCooling) * coolingScale;
            getHeatRef(x,y) = max(0.0f, getHeatRef(x,y) - decay);
        }
    }
}

void FireEffect::propagateUp() {
    // Simple heat propagation - heat rises straight up (no IMU tilt effects)

    // Default gravity: straight up (no horizontal tilt)
    float gravityX = 0.0f;  // No horizontal tilt
    float gravityY = 0.0f;  // No vertical tilt effect

    for (int y = HEIGHT - 1; y > 0; --y) {
        for (int x = 0; x < WIDTH; ++x) {
            float below      = getHeatRef(x, y - 1);
            float belowLeft  = getHeatRef((x + WIDTH - 1) % WIDTH, y - 1);
            float belowRight = getHeatRef((x + 1) % WIDTH, y - 1);

            // Adjust weights based on gravity direction
            float centerWeight = 1.4f;
            float leftWeight = 0.8f + gravityX * 0.3f;   // More weight if tilted left
            float rightWeight = 0.8f - gravityX * 0.3f;  // Less weight if tilted left

            // Ensure weights stay positive
            leftWeight = max(0.2f, leftWeight);
            rightWeight = max(0.2f, rightWeight);

            float weightedSum = below * centerWeight + belowLeft * leftWeight + belowRight * rightWeight;

            // Vertical propagation affected by gravity Y component
            float propagationRate = 3.1f - gravityY * 0.5f;  // Heat rises more when tilted
            propagationRate = constrain(propagationRate, 2.5f, 4.0f);

            getHeatRef(x, y) = weightedSum / propagationRate;
        }
    }
}

void FireEffect::injectSparks(float energy) {
    // Audio-responsive spark injection with balanced quiet/silence handling
    float minActivity = 0.05f; // Minimum activity level for quiet environments
    float adjustedEnergy = max(minActivity, energy);

    // Use gentler scaling - square root instead of square for better quiet response
    float energyScale = sqrt(adjustedEnergy); // Less aggressive than squaring
    float chanceScale = constrain(energyScale + params.audioSparkBoost * adjustedEnergy, 0.0f, 1.0f);

    int rows = max<int>(1, params.bottomRowsForSparks);
    rows = min(rows, HEIGHT);

    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            float roll = random(0, 10000) / 10000.0f;

            if (roll < params.sparkChance * chanceScale) {
                uint8_t h8 = random(params.sparkHeatMin, params.sparkHeatMax + 1);
                float h = h8 / 255.0f;

                // Heat boost proportional to actual energy level
                uint8_t boost8 = params.audioHeatBoostMax;
                float boost = (boost8 / 255.0f) * adjustedEnergy;

                float finalHeat = min(1.0f, h + boost);
                getHeatRef(x, 0) = max(getHeatRef(x, 0), finalHeat);
            }
        }
    }
}

uint32_t FireEffect::heatToColorRGB(float h) const {
    // Enhanced fire palette with more realistic color transitions
    // 0.00-0.15 : black -> dark red
    // 0.15-0.40 : dark red -> bright red
    // 0.40-0.70 : bright red -> orange
    // 0.70-0.90 : orange -> yellow
    // 0.90-1.00 : yellow -> bright white/blue

    // Clamp input
    if (h < 0.0f) h = 0.0f;
    if (h > 1.0f) h = 1.0f;

    // Add subtle flicker to make fire more dynamic
    float flicker = 1.0f + 0.05f * sin(millis() * 0.01f + h * 10.0f);
    h *= flicker;
    if (h > 1.0f) h = 1.0f;

    const float darkRedEnd = 0.15f;
    const float redEnd = 0.40f;
    const float orangeEnd = 0.70f;
    const float yellowEnd = 0.90f;

    uint8_t r, g, b;

    if (h <= darkRedEnd) {
        // black -> dark red
        float t = h / darkRedEnd;
        r = (uint8_t)(t * 120.0f + 0.5f);  // Dark red
        g = (uint8_t)(t * 15.0f + 0.5f);   // Tiny bit of green for warmth
        b = 0;
    } else if (h <= redEnd) {
        // dark red -> bright red
        float t = (h - darkRedEnd) / (redEnd - darkRedEnd);
        r = (uint8_t)(120 + t * 135.0f + 0.5f);  // 120->255
        g = (uint8_t)(15 + t * 25.0f + 0.5f);    // 15->40
        b = 0;
    } else if (h <= orangeEnd) {
        // bright red -> orange
        float t = (h - redEnd) / (orangeEnd - redEnd);
        r = 255;
        g = (uint8_t)(40 + t * 125.0f + 0.5f);   // 40->165
        b = (uint8_t)(t * 20.0f + 0.5f);         // 0->20
    } else if (h <= yellowEnd) {
        // orange -> yellow
        float t = (h - orangeEnd) / (yellowEnd - orangeEnd);
        r = 255;
        g = (uint8_t)(165 + t * 90.0f + 0.5f);   // 165->255
        b = (uint8_t)(20 + t * 30.0f + 0.5f);    // 20->50
    } else {
        // yellow -> bright white with blue
        float t = (h - yellowEnd) / (1.0f - yellowEnd);
        r = 255;
        g = 255;
        b = (uint8_t)(50 + t * 205.0f + 0.5f);   // 50->255
    }

    return leds.Color(r, g, b);
}
// Note: LED matrix is 16x8 around a cylinder; your physical mapping may differ.
// Here we assume x grows left→right, y grows top→bottom.
// If your strip is wired row-major starting at top-left, this is correct.
// If not, adapt this mapping to your wiring (non-serpentine assumed).
uint16_t FireEffect::xyToIndex(int x, int y) const {
    // Get current device config
    extern const DeviceConfig& config;

    x = ledMapper.wrapX(x);
    y = ledMapper.wrapY(y);

    // Use centralized LED mapper
    return ledMapper.getIndex(x, y);
}

void FireEffect::render() {
  for (int y = 0; y < HEIGHT; ++y) {
      int visY = HEIGHT - 1 - y; // if you flip vertically
      for (int x = 0; x < WIDTH; ++x) {
          float h = getHeatValue(x, y);                 // 0..1 float heat
          if (h < 0.0f) h = 0.0f; if (h > 1.0f) h = 1.0f;
          leds.setPixelColor(xyToIndex(x, visY), heatToColorRGB(h));
      }
  }
}

void FireEffect::show() {
  leds.show();
}

FireEffect::~FireEffect() {
    if (heat) {
        free(heat);
        heat = nullptr;
    }
}

