#include "FireEffect.h"
#include "SerialConsole.h"
#include "configs/DeviceConfig.h"

// Enhanced noise functions for more organic fire behavior
static float hashNoise(int x, int y, float t) {
    uint32_t n = x * 73856093u ^ y * 19349663u ^ (int)(t*1000);
    n = (n << 13) ^ n;
    return 1.0f - ((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffff) / 1073741824.0f;
}

// Multi-octave turbulence for more complex patterns
static float turbulence(float x, float y, float t, int octaves = 3) {
    float value = 0.0f;
    float amplitude = 1.0f;
    float frequency = 1.0f;

    for (int i = 0; i < octaves; i++) {
        value += hashNoise((int)(x * frequency), (int)(y * frequency), t * frequency) * amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return value / (2.0f - 1.0f / (1 << (octaves - 1)));
}

// Perlin-style smooth noise
static float smoothNoise(float x, float y, float t) {
    int ix = (int)x;
    int iy = (int)y;
    float fx = x - ix;
    float fy = y - iy;

    // Smooth interpolation (smoothstep)
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);

    float n00 = hashNoise(ix, iy, t);
    float n10 = hashNoise(ix + 1, iy, t);
    float n01 = hashNoise(ix, iy + 1, t);
    float n11 = hashNoise(ix + 1, iy + 1, t);

    float n0 = n00 * (1.0f - fx) + n10 * fx;
    float n1 = n01 * (1.0f - fx) + n11 * fx;

    return n0 * (1.0f - fy) + n1 * fy;
}

FireEffect::FireEffect(Adafruit_NeoPixel &strip, int width, int height)
    : leds(strip), WIDTH(width), HEIGHT(height), heat(nullptr), heatScratch(nullptr) {
    restoreDefaults();
}

void FireEffect::begin() {
    if (heat) {
        free(heat);
        heat = nullptr;
    }
    if (heatScratch) {
        free(heatScratch);
        heatScratch = nullptr;
    }

    heat = (float*)malloc(sizeof(float) * WIDTH * HEIGHT);
    if (!heat) {
        Serial.println(F("FireEffect: Failed to allocate heat buffer"));
        return;
    }

    for (int y = 0; y < HEIGHT; ++y)
        for (int x = 0; x < WIDTH;  ++x)
            H(x,y) = 0.0f;
}

void FireEffect::restoreDefaults() {
    params.baseCooling         = Defaults::BaseCooling;
    params.sparkHeatMin        = Defaults::SparkHeatMin;
    params.sparkHeatMax        = Defaults::SparkHeatMax;
    params.sparkChance         = Defaults::SparkChance;
    params.audioSparkBoost     = Defaults::AudioSparkBoost;
    params.audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
    params.coolingAudioBias    = Defaults::CoolingAudioBias;
    params.bottomRowsForSparks = Defaults::BottomRowsForSparks;
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

    // DISABLED: Wind lean effect not working as expected
    // addWindLean(dt);

    render();
}


void FireEffect::coolCells() {
    const float coolingScale = 0.5f / 255.0f;
    const int maxCooling = params.baseCooling + 1;

    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            // Simple random cooling
            const float decay = random(0, maxCooling) * coolingScale;
            H(x,y) = max(0.0f, H(x,y) - decay);
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
            float below      = H(x, y - 1);
            float belowLeft  = H((x + WIDTH - 1) % WIDTH, y - 1);
            float belowRight = H((x + 1) % WIDTH, y - 1);

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

            H(x, y) = weightedSum / propagationRate;
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
                H(x, 0) = max(H(x, 0), finalHeat);
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

    return leds.Color(g, r, b);
}
// Note: LED matrix is 16x8 around a cylinder; your physical mapping may differ.
// Here we assume x grows left→right, y grows top→bottom.
// If your strip is wired row-major starting at top-left, this is correct.
// If not, adapt this mapping to your wiring (non-serpentine assumed).
uint16_t FireEffect::xyToIndex(int x, int y) const {
    // Get current device config
    extern const DeviceConfig& config;

    x = (x % WIDTH + WIDTH) % WIDTH;
    y = (y % HEIGHT + HEIGHT) % HEIGHT;

    // Handle different matrix orientations and wiring patterns
    if (config.matrix.orientation == VERTICAL && WIDTH == 4 && HEIGHT == 16) {
        // Tube light: 4x16 zigzag pattern
        // Col 0: 0-15 (top to bottom)
        // Col 1: 31-16 (bottom to top)
        // Col 2: 32-47 (top to bottom)
        // Col 3: 63-48 (bottom to top)

        if (x % 2 == 0) {
            // Even columns (0,2): normal top-to-bottom
            return x * HEIGHT + y;
        } else {
            // Odd columns (1,3): bottom-to-top (reversed)
            return x * HEIGHT + (HEIGHT - 1 - y);
        }
    } else {
        // Fire totem: standard row-major mapping
        return y * WIDTH + x;
    }
}

void FireEffect::render() {
  for (int y = 0; y < HEIGHT; ++y) {
      int visY = HEIGHT - 1 - y; // if you flip vertically
      for (int x = 0; x < WIDTH; ++x) {
          float h = Hc(x, y);                 // 0..1 float heat
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
    if (heatScratch) {
        free(heatScratch);
        heatScratch = nullptr;
    }
}

