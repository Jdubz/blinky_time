#include "FireEffect.h"

FireEffect::FireEffect(Adafruit_NeoPixel &strip)
    : leds(strip) {
    restoreDefaults();
}

void FireEffect::begin() {
    for (int x = 0; x < WIDTH; x++) {
        for (int y = 0; y < HEIGHT; y++) {
            heat[x][y] = 0;
        }
    }
}

void FireEffect::update(float energy) {
    coolCells();
    propagateUp();
    injectSparks(energy);
    render();
}

void FireEffect::show() {
    leds.show();
}

void FireEffect::restoreDefaults() {
    params.cooling = Defaults::Cooling;
    params.sparkChance = Defaults::SparkChance;
    params.sparkHeatMin = Defaults::SparkHeatMin;
    params.sparkHeatMax = Defaults::SparkHeatMax;
    params.vuTopRowEnabled = Defaults::VuTopRowEnabled;
}

// --- Core Fire Functions ---

void FireEffect::coolCells() {
    for (int x = 0; x < WIDTH; x++) {
        for (int y = 0; y < HEIGHT; y++) {
            heat[x][y] -= params.cooling * random(0, 100) / 100.0f;
            if (heat[x][y] < 0) heat[x][y] = 0;
        }
    }
}

void FireEffect::propagateUp() {
    // copy heat upward with blur
    for (int x = 0; x < WIDTH; x++) {
        for (int y = HEIGHT - 1; y > 0; y--) {
            float below = heat[x][y - 1];
            float belowLeft = heat[(x + WIDTH - 1) % WIDTH][y - 1];
            float belowRight = heat[(x + 1) % WIDTH][y - 1];
            heat[x][y] = (below + belowLeft + belowRight) / 3.0f;
        }
    }
}

void FireEffect::injectSparks(float energy) {
    for (int x = 0; x < WIDTH; x++) {
        if (random(0, 1000) < params.sparkChance * 1000 * energy) {
            float sparkHeat = random(params.sparkHeatMin * 1000, 
                                     params.sparkHeatMax * 1000) / 1000.0f;
            heat[x][0] = sparkHeat;
        }
    }
}

void FireEffect::render() {
    for (int x = 0; x < WIDTH; x++) {
        for (int y = 0; y < HEIGHT; y++) {
            float h = constrain(heat[x][y], 0.0f, 1.0f);

            // Map heat to RGB (red–yellow–white palette)
            int r = (int)(h * 255);
            int g = (int)(min(h * 512, 255.0f));
            int b = (int)(min(h * 128, 255.0f));

            leds.setPixelColor(y * WIDTH + x, leds.Color(r, g, b));
        }
    }
}
