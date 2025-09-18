#include "FireEffect.h"
#include "SerialConsole.h"

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

    // Add simple wind lean effect without changing brightness
    addWindLean(dt);

    render();
}

void FireEffect::addWindLean(float dt) {
    // Simple horizontal wind lean effect - only shifts the visual appearance
    if (fabsf(windX) > 0.01f && WIDTH > 1) {
        // Console-controlled wind lean speed
        extern SerialConsole console;
        float dShift = windX * console.windSpeed * dt;

        // Allocate scratch buffer if needed
        if (!heatScratch) {
            heatScratch = (float*)malloc(sizeof(float) * WIDTH);
            if (!heatScratch) return;
        }

        // Simple horizontal shift for lean effect
        for (int y = 1; y < HEIGHT; ++y) {  // Skip bottom row to preserve sparks
            // Copy current row
            for (int x = 0; x < WIDTH; ++x) {
                heatScratch[x] = H(x, y);
            }

            // Apply subtle horizontal shift
            for (int x = 0; x < WIDTH; ++x) {
                float srcX = x - dShift;

                // Wrap around
                while (srcX < 0) srcX += WIDTH;
                while (srcX >= WIDTH) srcX -= WIDTH;

                int i0 = (int)srcX;
                int i1 = (i0 + 1) % WIDTH;
                float f = srcX - i0;

                H(x, y) = heatScratch[i0] * (1.0f - f) + heatScratch[i1] * f;
            }
        }
    }
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
    // Simple upward propagation without turbulence
    for (int y = HEIGHT - 1; y > 0; --y) {
        for (int x = 0; x < WIDTH; ++x) {
            float below      = H(x, y - 1);
            float belowLeft  = H((x + WIDTH - 1) % WIDTH, y - 1);
            float belowRight = H((x + 1) % WIDTH, y - 1);

            // Simple weighted average
            float weightedSum = below * 1.4f + belowLeft * 0.8f + belowRight * 0.8f;
            float totalWeight = 1.4f + 0.8f + 0.8f;

            // Apply heat rise with simple decay
            H(x, y) = weightedSum / 3.1f;
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
    x = (x % WIDTH + WIDTH) % WIDTH;
    y = (y % HEIGHT + HEIGHT) % HEIGHT;
    return y * WIDTH + x;
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

// ======== Enhanced FireEffect IMU Integration ========

void FireEffect::setTorchMotion(float windXIn, float windYIn, float stokeLevel,
                                float turbulence, float centrifugal, float flameBend,
                                float tiltAngleIn, float motionIntensityIn) {
    // Update basic motion state
    windX = windXIn;
    windY = windYIn;
    stoke = constrain(stokeLevel, 0.0f, 1.0f);

    // Update advanced motion effects
    turbulenceLevel = constrain(turbulence, 0.0f, 1.0f);
    centrifugalEffect = constrain(centrifugal, 0.0f, 2.0f);
    flameBendAngle = constrain(flameBend, 0.0f, 1.0f);
    tiltAngle = constrain(tiltAngleIn, 0.0f, 90.0f);
    motionIntensity = constrain(motionIntensityIn, 0.0f, 1.0f);

    // Adjust spark behavior based on motion
    sparkIntensity = 1.0f + motionIntensity * motionSparkFactor;
}

void FireEffect::setRotationalEffects(float spinMag, float centrifugalForce) {
    spinMagnitude = constrain(spinMag, 0.0f, 10.0f);
    centrifugalEffect = constrain(centrifugalForce, 0.0f, 2.0f);

    // Rotational motion affects spark spread and intensity
    sparkSpreadCols = constrain((int)(3 + spinMagnitude), 2, 6);
    sparkIntensity *= (1.0f + spinMagnitude * 0.2f);
}

void FireEffect::setInertialDrift(float driftX, float driftY) {
    inertiaDriftX = constrain(driftX, -5.0f, 5.0f);
    inertiaDriftY = constrain(driftY, -5.0f, 5.0f);

    // Inertial drift affects spark head movement
    sparkHeadX += inertiaDriftX * 0.1f;
    sparkHeadY += inertiaDriftY * 0.05f;

    // Keep spark head within bounds
    while (sparkHeadX < 0.0f) sparkHeadX += WIDTH;
    while (sparkHeadX >= WIDTH) sparkHeadX -= WIDTH;
    sparkHeadY = constrain(sparkHeadY, -2.0f, 2.0f);
}

void FireEffect::setFlameDirection(float direction, float bend) {
    flameDirection = direction;
    flameBendAngle = constrain(bend, 0.0f, 1.0f);
}

void FireEffect::setMotionTurbulence(float level) {
    turbulenceLevel = constrain(level, 0.0f, 1.0f);
}
