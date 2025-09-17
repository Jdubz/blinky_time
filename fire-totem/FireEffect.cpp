#include "FireEffect.h"

FireEffect::FireEffect(Adafruit_NeoPixel &strip, int width, int height)
    : leds(strip), WIDTH(width), HEIGHT(height), heat(nullptr) {
    restoreDefaults();
    const int N = leds.numPixels();
    heatVis = (float*)malloc(N * sizeof(float));
    if (heatVis) {
      for (int i = 0; i < N; ++i) heatVis[i] = 0.0f;    // init to 0.0
    }    lastMs = millis();
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

void FireEffect::update(float energy) {
    // --- Frame timing (for frame-rate independent smoothing) ---
    unsigned long now = millis();
    float dt = (now - lastMs) * 0.001f;
    if (dt < 0) dt = 0;
    lastMs = now;

    // Cooling bias by audio (negative = taller flames for loud parts)
    int16_t coolingBias = params.coolingAudioBias; // int8_t promoted
    int cooling = params.baseCooling + coolingBias; // can go below 0; clamp in coolCells

    coolCells();

    propagateUp();

    injectSparks(energy);

    // --- visual smoothing (only affects rendering, never the physics heat[]) ---
    if (heatVis && params.decayTau > 0.0f) {
      const int N = leds.numPixels();
      float aR = 1.0f - expf(-dt / params.decayTau);   // 0..1 release coefficient
      if (aR < 0.0f) aR = 0.0f;
      if (aR > 1.0f) aR = 1.0f;

      for (int i = 0; i < N; ++i) {
        float h  = heat[i];       // current physics (0..1)
        float hv = heatVis[i];    // last visual (0..1)
        if (h >= hv) {
          heatVis[i] = h;         // instant attack
        } else {
          heatVis[i] = hv + (h - hv) * aR;  // eased release
        }
        // clamp to [0,1] just in case
        if (heatVis[i] < 0.0f) heatVis[i] = 0.0f;
        if (heatVis[i] > 1.0f) heatVis[i] = 1.0f;
      }
    } else if (heatVis) {
      // no smoothing -> mirror physics
      const int N = leds.numPixels();
      for (int i = 0; i < N; ++i) heatVis[i] = heat[i];
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

    for (int y = 0; y < rows; ++y) { // bottom rows for sparks (y=0 is top in render; here we treat y=0 as source row in grid logic)
        // NOTE: our render maps y=0 to top; to keep intuitive “bottom source”, we’ll inject at y=0 here,
        // and the render will map so that high y becomes visually lower (see heatToColor/render comment).
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
const int N = leds.numPixels();
  const float* src = heatVis ? heatVis : heat;  // fallback if alloc failed

  for (int i = 0; i < N; ++i) {
    float h = src[i];
    uint32_t c = heatToColorRGB(h);   // your existing Doom-style palette mapper
    leds.setPixelColor(i, c);
  }
  leds.show();}

FireEffect::~FireEffect() {
  if (heatVis) { free(heatVis); heatVis = nullptr; }
}

