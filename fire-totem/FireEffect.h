#pragma once

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "TotemDefaults.h"
#include "Globals.h"

struct FireParams {
    // Matches Defaults.*
    uint8_t baseCooling         = Defaults::BaseCooling;
    uint8_t sparkHeatMin        = Defaults::SparkHeatMin;
    uint8_t sparkHeatMax        = Defaults::SparkHeatMax;
    float   sparkChance         = Defaults::SparkChance;
    float   audioSparkBoost     = Defaults::AudioSparkBoost;
    uint8_t audioHeatBoostMax   = Defaults::AudioHeatBoostMax;
    int8_t  coolingAudioBias    = Defaults::CoolingAudioBias;
    uint8_t bottomRowsForSparks = Defaults::BottomRowsForSparks;
    uint8_t transientHeatMax    = Defaults::TransientHeatMax;
};

class FireEffect {
public:
    // Constructors: support both pointer & reference forms seen in .ino
    FireEffect(Adafruit_NeoPixel &strip, int width = 4, int height = 15);
    FireEffect(Adafruit_NeoPixel *strip, int width = 4, int height = 15)
        : FireEffect(*strip, width, height) {}
    ~FireEffect();

    void begin();
    void update(float energy, float hit);
    // Back-compat: ignore dx/dy so existing .ino compiles
    void update(float energy, float /*dx*/, float /*dy*/, float hit) { update(energy, hit); }

    void show();
    void render(); // public because .ino calls it
    void restoreDefaults();

    // For VU meter / external mapping
    uint16_t xyToIndex(int x, int y) const;

    // Heat access for visualization
    float getHeat(int x, int y) const {
        if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT || !heat) return 0.0f;
        return Hc(x, y);
    }

    // Make params public so SerialConsole can read/write them
    FireParams params;


private:
    Adafruit_NeoPixel &leds;
    int WIDTH;
    int HEIGHT;
      // frame timing for dt
    unsigned long lastUpdateMs = 0; 

    // Retro fire heat grid
    // [x][y], y=0 is top row for rendering; we still treat bottom (y=HEIGHT-1) as the "fire source".
    // We’ll map carefully in render().
    float *heat; // flat allocation: WIDTH*HEIGHT

    inline float& H(int x, int y) { return heat[y * WIDTH + x]; }
    inline const float& Hc(int x, int y) const { return heat[y * WIDTH + x]; }

    void coolCells();
    void propagateUp();
    void injectSparks(float energy);
    void addWindLean(float dt);

    // palette
    uint32_t heatToColorRGB(float h) const;




    float* heatScratch = nullptr;    // temp row buffer for advection
};

