#include "FireVisualEffect.h"
#include "TotemDefaults.h"
#include <Arduino.h>

FireVisualEffect::FireVisualEffect() 
    : width_(0), height_(0), heat_(nullptr), lastUpdateMs_(0) {
    restoreDefaults();
}

FireVisualEffect::~FireVisualEffect() {
    if (heat_) {
        free(heat_);
        heat_ = nullptr;
    }
}

void FireVisualEffect::begin(int width, int height) {
    width_ = width;
    height_ = height;
    
    if (heat_) {
        free(heat_);
        heat_ = nullptr;
    }
    
    heat_ = (float*)malloc(sizeof(float) * width * height);
    if (!heat_) {
        Serial.println(F("FireVisualEffect: Failed to allocate heat buffer"));
        return;
    }
    
    clearHeat();
    lastUpdateMs_ = 0;
}

void FireVisualEffect::restoreDefaults() {
    // Use TotemDefaults for consistent behavior
    params.baseCooling         = Defaults::BaseCooling;
    params.sparkHeatMin        = Defaults::SparkHeatMin;
    params.sparkHeatMax        = Defaults::SparkHeatMax;
    params.sparkChance         = Defaults::SparkChance;
    params.audioSparkBoost     = Defaults::AudioSparkBoost;
    params.audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
    params.coolingAudioBias    = Defaults::CoolingAudioBias;
    params.bottomRowsForSparks = Defaults::BottomRowsForSparks;
    params.transientHeatMax    = Defaults::TransientHeatMax;
}

void FireVisualEffect::update(float energy, float hit) {
    if (!heat_) return;
    
    // Balanced ember floor - allows quiet adaptation but reduces silence activity
    float emberFloor = 0.03f; // 3% energy floor
    float boostedEnergy = max(emberFloor, energy * (1.0f + hit * (params.transientHeatMax / 255.0f)));

    // Frame timing
    unsigned long nowMs = millis();
    float dt = (lastUpdateMs_ == 0) ? 0.0f : (nowMs - lastUpdateMs_) * 0.001f;
    lastUpdateMs_ = nowMs;

    // --- COOLING PHASE ---
    float baseCoolingPerSecond = params.baseCooling;
    float audioCoolingBias = params.coolingAudioBias * boostedEnergy;
    float totalCoolingRate = baseCoolingPerSecond + audioCoolingBias;
    
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            float& heat = getHeatRef(x, y);
            heat = max(0.0f, heat - totalCoolingRate * dt);
        }
    }

    // --- HEAT DIFFUSION PHASE ---
    // Temporary buffer for diffusion to avoid feedback loops
    static float* tempHeat = nullptr;
    static int tempBufferSize = 0;
    
    int requiredSize = width_ * height_;
    if (!tempHeat || tempBufferSize < requiredSize) {
        if (tempHeat) free(tempHeat);
        tempHeat = (float*)malloc(sizeof(float) * requiredSize);
        tempBufferSize = requiredSize;
    }
    
    if (tempHeat) {
        // Copy current heat to temp buffer
        memcpy(tempHeat, heat_, sizeof(float) * requiredSize);
        
        // Diffuse heat upward
        for (int y = 1; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                int tempIndex = y * width_ + x;
                int belowIndex = (y - 1) * width_ + x;
                
                float heatBelow = tempHeat[belowIndex];
                float& currentHeat = heat_[tempIndex];
                
                // Mix heat from below
                float diffusionAmount = heatBelow * 0.1f * dt * 60.0f; // Scale by framerate
                currentHeat = min(255.0f, currentHeat + diffusionAmount);
            }
        }
    }

    // --- SPARK GENERATION PHASE ---
    float totalSparkChance = params.sparkChance + boostedEnergy * params.audioSparkBoost;
    
    for (int y = 0; y < params.bottomRowsForSparks; ++y) {
        for (int x = 0; x < width_; ++x) {
            if (random(0, 1000) < (totalSparkChance * 1000.0f)) {
                float sparkHeat = random(params.sparkHeatMin, params.sparkHeatMax + 1);
                sparkHeat += boostedEnergy * params.audioHeatBoostMax;
                
                getHeatRef(x, y) = min(255.0f, sparkHeat);
            }
        }
    }
}

void FireVisualEffect::render(EffectMatrix& matrix) {
    if (!heat_) return;
    
    int matrixWidth = matrix.getWidth();
    int matrixHeight = matrix.getHeight();
    
    // Ensure matrix dimensions match our effect dimensions
    if (matrixWidth != width_ || matrixHeight != height_) {
        Serial.println(F("FireVisualEffect: Matrix dimension mismatch"));
        return;
    }
    
    for (int y = 0; y < height_; ++y) {
        int visY = height_ - 1 - y; // Flip vertically for upward flames
        for (int x = 0; x < width_; ++x) {
            float heat = getHeatValue(x, y);
            RGB color = heatToColor(heat / 255.0f); // Normalize to 0-1
            matrix.setPixel(x, visY, color);
        }
    }
}

float& FireVisualEffect::getHeatRef(int x, int y) {
    x = wrapX(x);
    y = wrapY(y);
    return heat_[y * width_ + x];
}

float FireVisualEffect::getHeatValue(int x, int y) const {
    x = wrapX(x);
    y = wrapY(y);
    return heat_[y * width_ + x];
}

RGB FireVisualEffect::heatToColor(float h) const {
    // Enhanced fire palette with more realistic color transitions
    // Same as original FireEffect but returns RGB struct
    
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

    return RGB(r, g, b);
}

int FireVisualEffect::wrapX(int x) const {
    while (x < 0) x += width_;
    while (x >= width_) x -= width_;
    return x;
}

int FireVisualEffect::wrapY(int y) const {
    while (y < 0) y += height_;
    while (y >= height_) y -= height_;
    return y;
}

// Testing helpers
void FireVisualEffect::setHeat(int x, int y, float heat) {
    if (heat_ && x >= 0 && x < width_ && y >= 0 && y < height_) {
        heat_[y * width_ + x] = heat;
    }
}

float FireVisualEffect::getHeat(int x, int y) const {
    if (heat_ && x >= 0 && x < width_ && y >= 0 && y < height_) {
        return heat_[y * width_ + x];
    }
    return 0.0f;
}

void FireVisualEffect::clearHeat() {
    if (heat_) {
        for (int i = 0; i < width_ * height_; i++) {
            heat_[i] = 0.0f;
        }
    }
}