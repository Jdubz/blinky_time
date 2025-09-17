#include "FireEffect.h"

// simple hash noise (fast, repeatable)
static float hashNoise(int x, int y, float t) {
    uint32_t n = x * 73856093u ^ y * 19349663u ^ (int)(t*1000);
    n = (n << 13) ^ n;
    return 1.0f - ((n * (n * n * 15731u + 789221u) + 1376312589u) & 0x7fffffff) / 1073741824.0f;
}

FireEffect::FireEffect(Adafruit_NeoPixel &strip, int WIDTH, int height)
    : leds(strip), WIDTH(WIDTH), HEIGHT(height), heat(nullptr) {
    restoreDefaults();
}

void FireEffect::begin() {
    if (heat) { free(heat); heat = nullptr; }
    heat = (float*)malloc(sizeof(float) * WIDTH * HEIGHT);
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
    float pExtra = fabsf(windX) * 0.12f;
    if (pExtra > 0.35f) pExtra = 0.35f;

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
      }

      if (heatScratch) {
        auto advectRowWrap = [](const float* inRow, float* outRow, int W, float s) {
          for (int x = 0; x < W; ++x) {
            float srcX = x - s;
            while (srcX < 0)    srcX += W;
            while (srcX >= W)   srcX -= W;
            int   i0 = (int)srcX;
            int   i1 = (i0 + 1 == W) ? 0 : (i0 + 1);
            float f  = srcX - i0;
            float v  = inRow[i0] * (1.0f - f) + inRow[i1] * f;
            if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
            outRow[x] = v;
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

          // If width is small (<=32), use stack; otherwise use heap for out row too.
          if (WIDTH <= 32) {
            outRow = tmpRow;
          } else {
            outRow = (float*)alloca(sizeof(float) * WIDTH); // safe for moderate widths
          }

          advectRowWrap(heatScratch, outRow, WIDTH, dShift);
          for (int x = 0; x < WIDTH; ++x) {
            heat[ xyToIndex(x, y) ] = outRow[x];
          }
        }
      }
    }

    render();
}

void FireEffect::coolCells() {
    // Port of "cooling" using 0..255 style; random cooling per cell
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            // Random cooling scaled from baseCooling; map to ~0..~0.1 subtraction
            // Maintain compatibility with uint8_t cooling by dividing by 255.
            float decay = (random(0, params.baseCooling + 1) / 255.0f) * 0.5f; // tune factor
            H(x,y) -= decay;
            if (H(x,y) < 0.0f) H(x,y) = 0.0f;
        }
    }
}

void FireEffect::propagateUp() {
    // classic doom-like upward blur from bottom to top
    for (int y = HEIGHT - 1; y > 0; --y) {
        for (int x = 0; x < WIDTH; ++x) {
            float below      = H(x, y - 1);
            float belowLeft  = H((x + WIDTH - 1) % WIDTH, y - 1);
            float belowRight = H((x + 1) % WIDTH, y - 1);
            // average & slight decay
            H(x, y) = (below + belowLeft + belowRight) / 3.05f; // small loss to keep from saturating
        }
    }
    // Top row naturally dissipates via cooling
}

void FireEffect::injectSparks(float energy) {
    // audioEnergy scales both chance and heat
    float chanceScale = constrain(energy + params.audioSparkBoost * energy, 0.0f, 1.0f);

    int rows = max<int>(1, params.bottomRowsForSparks);
    rows = min(rows, HEIGHT);

    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            float roll = random(0, 10000) / 10000.0f;
            if (roll < params.sparkChance * chanceScale) {
                uint8_t h8 = random(params.sparkHeatMin, params.sparkHeatMax + 1);
                float h = h8 / 255.0f;

                // add audio heat boost
                uint8_t boost8 = params.audioHeatBoostMax;
                float boost = (boost8 / 255.0f) * energy;
                H(x, 0) = min(1.0f, h + boost);
            }
        }
    }
}

uint32_t FireEffect::heatToColorRGB(float h) const {
    // Doom-style palette using heat in [0,1]:
    //  0.00–0.33 : black -> red
    //  0.33–0.85 : red   -> yellow (green ramps in)
    //  0.85–1.00 : yellow-> white  (blue ramps in near the very top)

    // Clamp input
    if (h < 0.0f) h = 0.0f;
    if (h > 1.0f) h = 1.0f;

    const float redEnd    = 0.33f;
    const float yellowEnd = 0.85f;  // push white to the very top so you don't get strobing

    uint8_t r, g, b;

    if (h <= redEnd) {
        // black -> red
        float t = h / redEnd;                      // 0..1
        r = (uint8_t)(t * 255.0f + 0.5f);
        g = 0;
        b = 0;
    } else if (h <= yellowEnd) {
        // red -> yellow (green ramps in, blue stays 0)
        float t = (h - redEnd) / (yellowEnd - redEnd); // 0..1
        r = 255;
        g = (uint8_t)(t * 255.0f + 0.5f);
        b = 0;
    } else {
        // yellow -> white (blue ramps in only near the very top)
        float t = (h - yellowEnd) / (1.0f - yellowEnd); // 0..1
        r = 255;
        g = 255;
        b = (uint8_t)(t * 255.0f + 0.5f);
        // Optional: soften white caps if you still see harsh flashes
        // b = (uint8_t)min(220, (int)(t * 255.0f + 0.5f));
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
  if (heatScratch) { free(heatScratch); heatScratch = nullptr; }
}
