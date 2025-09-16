#include "FireEffect.h"

FireEffect::FireEffect(Adafruit_NeoPixel &strip, int width, int height)
    : leds(strip), WIDTH(width), HEIGHT(height), heat(nullptr) {
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
    params.vuTopRowEnabled     = Defaults::VuTopRowEnabled;
}

void FireEffect::update(float energy) {
    // Cooling bias by audio (negative = taller flames for loud parts)
    int16_t coolingBias = params.coolingAudioBias; // int8_t promoted
    int cooling = params.baseCooling + coolingBias; // can go below 0; clamp in coolCells

    coolCells();

    propagateUp();

    injectSparks(energy);

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

uint32_t FireEffect::heatToColor(float h) const {
    // map heat 0..1 to red-yellow-white
    h = constrain(h, 0.0f, 1.0f);
    uint8_t r = (uint8_t)(h * 255.0f);
    uint8_t g = (uint8_t)min(h * 512.0f, 255.0f);
    uint8_t b = (uint8_t)min(h * 128.0f, 255.0f);
    return leds.Color(r,g,b);
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
    // Render the grid to LEDs.
    // IMPORTANT: Our propagation used y=0 as the "source" row.
    // For visual correctness (fire rises upward), we flip vertically in render,
    // so the brightest new sparks appear at the bottom of the cylinder.
    for (int y = 0; y < HEIGHT; ++y) {
        int visY = HEIGHT - 1 - y; // flip so y=0 (source) shows at bottom
        for (int x = 0; x < WIDTH; ++x) {
            uint32_t c = heatToColor(Hc(x, y));
            leds.setPixelColor(xyToIndex(x, visY), c);
        }
    }
}

void FireEffect::show() {
    leds.show();
}
