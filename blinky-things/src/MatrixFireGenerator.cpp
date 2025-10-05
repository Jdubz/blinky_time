#include "../generators/MatrixFireGenerator.h"
#include <Arduino.h>

MatrixFireGenerator::MatrixFireGenerator(int w, int h)
    : width(w), height(h), heat(nullptr), lastUpdateMs(0) {

    // Allocate heat buffer
    heat = (float*)malloc(sizeof(float) * width * height);
    if (!heat) {
        Serial.println(F("MatrixFireGenerator: Failed to allocate heat buffer"));
        return;
    }

    reset();
}

MatrixFireGenerator::~MatrixFireGenerator() {
    if (heat) {
        free(heat);
        heat = nullptr;
    }
}

void MatrixFireGenerator::reset() {
    if (!heat) return;

    // Clear heat grid
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            getHeatRef(x, y) = 0.0f;
        }
    }

    lastUpdateMs = 0;
}

void MatrixFireGenerator::generate(EffectMatrix& matrix, float energy, float hit) {
    if (!heat || matrix.getWidth() != width || matrix.getHeight() != height) {
        return;
    }

    // Balanced ember floor - allows quiet adaptation but reduces silence activity
    float emberFloor = 0.03f; // 3% energy floor
    float boostedEnergy = max(emberFloor, energy * (1.0f + hit * (params.transientHeatMax / 255.0f)));

    // Frame timing
    unsigned long nowMs = millis();
    float dt = (lastUpdateMs == 0) ? 0.0f : (nowMs - lastUpdateMs) * 0.001f;
    lastUpdateMs = nowMs;

    // Update fire simulation
    coolCells();
    propagateUp();
    injectSparks(boostedEnergy);

    // Convert heat to colors and populate matrix
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float h = getHeatValue(x, y);
            h = constrain(h, 0.0f, 1.0f);
            uint32_t color = heatToColorRGB(h);
            matrix.setPixel(x, y, color);
        }
    }
}

float MatrixFireGenerator::getHeat(int x, int y) const {
    if (x < 0 || x >= width || y < 0 || y >= height || !heat) {
        return 0.0f;
    }
    return getHeatValue(x, y);
}

void MatrixFireGenerator::coolCells() {
    const float coolingScale = 0.5f / 255.0f;
    const int maxCooling = params.baseCooling + 1;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Simple random cooling
            const float decay = random(0, maxCooling) * coolingScale;
            getHeatRef(x, y) = max(0.0f, getHeatRef(x, y) - decay);
        }
    }
}

void MatrixFireGenerator::propagateUp() {
    // Heat propagation - heat rises straight up
    for (int y = height - 1; y > 0; --y) {
        for (int x = 0; x < width; ++x) {
            float below      = getHeatRef(x, y - 1);
            float belowLeft  = getHeatRef((x + width - 1) % width, y - 1);
            float belowRight = getHeatRef((x + 1) % width, y - 1);

            // Standard fire propagation weights
            float centerWeight = 1.4f;
            float leftWeight = 0.8f;
            float rightWeight = 0.8f;

            float weightedSum = below * centerWeight + belowLeft * leftWeight + belowRight * rightWeight;
            float propagationRate = 3.1f;

            getHeatRef(x, y) = weightedSum / propagationRate;
        }
    }
}

void MatrixFireGenerator::injectSparks(float energy) {
    // Audio-responsive spark injection
    float minActivity = 0.05f; // Minimum activity level
    float adjustedEnergy = max(minActivity, energy);

    // Use gentler scaling - square root for better quiet response
    float energyScale = sqrt(adjustedEnergy);
    float chanceScale = constrain(energyScale + params.audioSparkBoost * adjustedEnergy, 0.0f, 1.0f);

    int rows = max<int>(1, params.bottomRowsForSparks);
    rows = min(rows, height);

    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < width; ++x) {
            float roll = random(0, 10000) / 10000.0f;

            if (roll < params.sparkChance * chanceScale) {
                uint8_t h8 = random(params.sparkHeatMin, params.sparkHeatMax + 1);
                float h = h8 / 255.0f;

                // Heat boost proportional to energy level
                uint8_t boost8 = params.audioHeatBoostMax;
                float boost = (boost8 / 255.0f) * adjustedEnergy;

                float finalHeat = min(1.0f, h + boost);
                getHeatRef(x, 0) = max(getHeatRef(x, 0), finalHeat);
            }
        }
    }
}

uint32_t MatrixFireGenerator::heatToColorRGB(float h) const {
    // Enhanced fire palette with realistic color transitions
    if (h < 0.0f) h = 0.0f;
    if (h > 1.0f) h = 1.0f;

    // Add subtle flicker
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
        r = (uint8_t)(t * 120.0f + 0.5f);
        g = (uint8_t)(t * 15.0f + 0.5f);
        b = 0;
    } else if (h <= redEnd) {
        // dark red -> bright red
        float t = (h - darkRedEnd) / (redEnd - darkRedEnd);
        r = (uint8_t)(120 + t * 135.0f + 0.5f);
        g = (uint8_t)(15 + t * 25.0f + 0.5f);
        b = 0;
    } else if (h <= orangeEnd) {
        // bright red -> orange
        float t = (h - redEnd) / (orangeEnd - redEnd);
        r = 255;
        g = (uint8_t)(40 + t * 125.0f + 0.5f);
        b = (uint8_t)(t * 20.0f + 0.5f);
    } else if (h <= yellowEnd) {
        // orange -> yellow
        float t = (h - orangeEnd) / (yellowEnd - orangeEnd);
        r = 255;
        g = (uint8_t)(165 + t * 90.0f + 0.5f);
        b = (uint8_t)(20 + t * 30.0f + 0.5f);
    } else {
        // yellow -> bright white with blue
        float t = (h - yellowEnd) / (1.0f - yellowEnd);
        r = 255;
        g = 255;
        b = (uint8_t)(50 + t * 205.0f + 0.5f);
    }

    // Return RGB packed color (not using Adafruit_NeoPixel::Color here)
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
