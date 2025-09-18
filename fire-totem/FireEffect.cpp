#include "FireEffect.h"

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
    float emberFloor = 0.05f; // 5% energy floor
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

    // ---- IMU integration: wind-biased spark + upward stoke ----
    // 1) Orientation: which way is up
    const int baseRowStart = 0;
    const int baseRowStep  = +1;

    // 2) Stoke: inject a small extra heat in the bottom N rows
    if (stoke > 0.0f) {
      // Reuse your AudioHeatBoostMax as a sane scale; convert to 0..1
      const float boost = stoke * (Defaults::AudioHeatBoostMax / 255.0f);
      const int rows = Defaults::BottomRowsForSparks;
      for (int y = 0; y < rows; ++y) {
        const int row = baseRowStart + baseRowStep * y;
        for (int x = 0; x < WIDTH; ++x) {
          const int idx = xyToIndex(x, row);
          float h = heat[idx] + boost;
          heat[idx] = (h > 1.0f) ? 1.0f : h;
        }
      }
    }

    // 3) Wind: drift a “spark head” around the cylinder; spawn near it occasionally
    //    (keeps everything gentle—doesn't alter transport/cooling)
    static unsigned long lastWindMs = 0;
    float dtWind = (lastWindMs == 0) ? 0.016f : (nowMs - lastWindMs) * 0.001f;
    lastWindMs = nowMs;

    // drift head by windX (IMU wind already ~cells/sec-ish)
    sparkHeadX += windX * windColsPerSec * dtWind;

    // wrap into [0, WIDTH)
    while (sparkHeadX < 0.0f) sparkHeadX += WIDTH;
    while (sparkHeadX >= WIDTH) sparkHeadX -= WIDTH;

    // probability to add an extra wind-biased spark this frame
    // scales with |windX|; clamp to something subtle
    const float windSparkFactor = 0.12f;
    const float maxWindSparkProb = 0.35f;
    float pExtra = constrain(fabsf(windX) * windSparkFactor, 0.0f, maxWindSparkProb);

    // quick RNG in [0,1)
    if (random(1000) < (int)(pExtra * 1000.0f)) {
      int xCenter = (int)(sparkHeadX + 0.5f);
      int xSpawn  = xCenter + random(-sparkSpreadCols, sparkSpreadCols + 1);
      if (xSpawn < 0) xSpawn += WIDTH;
      if (xSpawn >= WIDTH) xSpawn -= WIDTH;

      const int row = baseRowStart; // hottest row at the base end
      const int idx = xyToIndex(xSpawn, row);

      // pick a heat pulse within your configured spark range; convert to 0..1
      int sparkByte = random(Defaults::SparkHeatMin, (int)Defaults::SparkHeatMax + 1);
      float spark = sparkByte / 255.0f;

      // small audio coupling so louder moments bias brighter windsparks
      spark *= (1.0f + Defaults::AudioSparkBoost * boostedEnergy);
      float h = heat[idx] + spark;
      heat[idx] = (h > 1.0f) ? 1.0f : h;
    }

    // ---- Lateral wind advection (visual “lean”) ----
    if (fabsf(windX) > 1e-4f && WIDTH > 1) {
      // how far to shift this frame, in columns
      // positive windX shifts content to the right; negative to the left
      float dShift = windX * windColsPerSec * dt;   // dt = seconds since last frame

      // lazy-allocate a scratch row if needed
      if (!heatScratch) {
        heatScratch = (float*)malloc(sizeof(float) * WIDTH);
        if (!heatScratch) {
          Serial.println(F("FireEffect: Failed to allocate scratch buffer"));
          return;
        }
      }

      // Pre-calculate modulo for better performance
      auto advectRowWrap = [](const float* inRow, float* outRow, int W, float s) {
          const float fW = (float)W;
          for (int x = 0; x < W; ++x) {
            float srcX = x - s;
            // Efficient wrap using fmod instead of while loops
            srcX = srcX - fW * floorf(srcX / fW);
            if (srcX < 0.0f) srcX += fW;

            const int i0 = (int)srcX;
            const int i1 = (i0 + 1 >= W) ? 0 : (i0 + 1);
            const float f = srcX - i0;
            const float v = inRow[i0] * (1.0f - f) + inRow[i1] * f;
            outRow[x] = constrain(v, 0.0f, 1.0f);
          }
        };
        for (int y = 0; y < HEIGHT; ++y) {
          // copy current row to scratch
          for (int x = 0; x < WIDTH; ++x) {
            heatScratch[x] = heat[ xyToIndex(x, y) ];
          }
          // advect into place
          float tmpRow[32]; // if width <= 32; otherwise use a second heap buffer
          float* outRow = nullptr;

          // Use stack buffer for small widths, heap for larger
          static float* largeTmpRow = nullptr;
          if (WIDTH <= 32) {
            outRow = tmpRow;
          } else {
            if (!largeTmpRow) {
              largeTmpRow = (float*)malloc(sizeof(float) * WIDTH);
              if (!largeTmpRow) {
                Serial.println(F("FireEffect: Failed to allocate temp row buffer"));
                return;
              }
            }
            outRow = largeTmpRow;
          }

          advectRowWrap(heatScratch, outRow, WIDTH, dShift);
          for (int x = 0; x < WIDTH; ++x) {
            heat[ xyToIndex(x, y) ] = outRow[x];
          }
        }
      }

    render();
}

void FireEffect::coolCells() {
    float time = millis() * 0.001f;
    const float coolingScale = 0.5f / 255.0f;
    const int maxCooling = params.baseCooling + 1;

    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            // Base random cooling
            float baseCooling = random(0, maxCooling) * coolingScale;

            // Add turbulent cooling variations for more organic decay
            float turbCooling = turbulence(x * 0.3f, y * 0.5f, time * 0.8f) * 0.02f;

            // Height-based cooling (flames cool more at the top)
            float heightFactor = 1.0f + (float)y / HEIGHT * 0.5f;

            // Apply enhanced cooling with flickering
            float totalCooling = (baseCooling + turbCooling) * heightFactor;

            // Add subtle pulsing to create flame breathing effect
            float pulse = 1.0f + 0.15f * sin(time * 3.0f + x * 0.5f + y * 0.3f);
            totalCooling *= pulse;

            H(x,y) = max(0.0f, H(x,y) - totalCooling);
        }
    }
}

void FireEffect::propagateUp() {
    float time = millis() * 0.001f; // Time in seconds

    // Enhanced upward propagation with turbulence
    for (int y = HEIGHT - 1; y > 0; --y) {
        for (int x = 0; x < WIDTH; ++x) {
            float below      = H(x, y - 1);
            float belowLeft  = H((x + WIDTH - 1) % WIDTH, y - 1);
            float belowRight = H((x + 1) % WIDTH, y - 1);

            // Add turbulence to create more organic flame shapes
            float baseTurbOffset = turbulence(x * 0.5f, y * 0.3f, time * 2.0f) * 0.3f - 0.15f;

            // Add motion-induced turbulence for torch realism
            float motionTurbOffset = turbulenceLevel * turbulenceScale *
                                   (smoothNoise(x * 0.7f, y * 0.4f, time * 3.0f) - 0.5f);

            float totalTurbOffset = baseTurbOffset + motionTurbOffset;

            // Calculate weighted average with enhanced turbulence influence
            float centerWeight = 1.4f + totalTurbOffset;
            float sideWeight = 0.8f - totalTurbOffset * 0.5f;

            float totalWeight = centerWeight + 2.0f * sideWeight;
            float weightedSum = below * centerWeight + belowLeft * sideWeight + belowRight * sideWeight;

            // Apply heat rise with enhanced decay and turbulence
            float baseDecay = 3.1f;
            float turbulentDecay = baseDecay + smoothNoise(x * 0.8f, y * 0.4f, time * 1.5f) * 0.4f;

            H(x, y) = weightedSum / turbulentDecay;

            // Enhanced horizontal drift with motion and flame direction effects
            float baseDrift = smoothNoise(x * 0.2f, y * 0.6f, time * 1.0f) * 0.1f;

            // Add flame direction bias from motion
            float directionBias = 0.0f;
            if (flameBendAngle > 0.1f) {
                float directionRad = flameDirection * M_PI / 180.0f;
                directionBias = cos(directionRad) * flameBendAngle * 0.15f;
            }

            // Add inertial drift effects
            float inertiaBias = (inertiaDriftX / WIDTH) * 0.1f;

            // Add centrifugal effects for rotation
            float centrifugalBias = 0.0f;
            if (centrifugalEffect > 0.1f && y > HEIGHT/2) {
                // Centrifugal force spreads flames outward at the top
                float radiusFromCenter = abs(x - WIDTH/2) / (WIDTH/2.0f);
                centrifugalBias = centrifugalEffect * radiusFromCenter * 0.08f;
                if (x > WIDTH/2) centrifugalBias = abs(centrifugalBias);
                else centrifugalBias = -abs(centrifugalBias);
            }

            float totalDrift = baseDrift + directionBias + inertiaBias + centrifugalBias;

            if (totalDrift > 0.03f && x < WIDTH - 1) {
                float mixFactor = min(totalDrift * 2.0f, 0.3f);
                H(x, y) = H(x, y) * (1.0f - mixFactor) + H(x + 1, y) * mixFactor;
            } else if (totalDrift < -0.03f && x > 0) {
                float mixFactor = min(-totalDrift * 2.0f, 0.3f);
                H(x, y) = H(x, y) * (1.0f - mixFactor) + H(x - 1, y) * mixFactor;
            }
        }
    }
    // Top row naturally dissipates via cooling
}

void FireEffect::injectSparks(float energy) {
    float time = millis() * 0.001f;

    // Enhanced audio energy scaling with more dynamic response
    float chanceScale = constrain(energy + params.audioSparkBoost * energy, 0.0f, 1.0f);

    int rows = max<int>(1, params.bottomRowsForSparks);
    rows = min(rows, HEIGHT);

    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            float roll = random(0, 10000) / 10000.0f;

            // Enhanced spatial clustering with motion effects
            float clusterNoise = smoothNoise(x * 0.6f, y * 0.4f, time * 0.5f);

            // Motion-based spark enhancement
            float motionSparkBoost = 1.0f + motionIntensity * 0.5f;
            float intensityBasedChance = sparkIntensity * motionSparkBoost;

            float clusterChance = params.sparkChance * chanceScale *
                                (0.5f + clusterNoise) * intensityBasedChance;

            if (roll < clusterChance) {
                uint8_t h8 = random(params.sparkHeatMin, params.sparkHeatMax + 1);
                float h = h8 / 255.0f;

                // Enhanced heat boost with turbulence variation
                uint8_t boost8 = params.audioHeatBoostMax;
                float boost = (boost8 / 255.0f) * energy;

                // Add turbulent spark intensity variations
                float turbVariation = turbulence(x * 0.8f, y * 0.6f, time * 4.0f) * 0.3f;
                h += turbVariation;

                // Create spark clusters with neighboring influence
                float finalHeat = min(1.0f, h + boost);
                H(x, 0) = max(H(x, 0), finalHeat);

                // Add slight influence to neighboring cells for clustering
                if (finalHeat > 0.7f) {
                    if (x > 0) H(x - 1, 0) = max(H(x - 1, 0), finalHeat * 0.3f);
                    if (x < WIDTH - 1) H(x + 1, 0) = max(H(x + 1, 0), finalHeat * 0.3f);
                }
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
