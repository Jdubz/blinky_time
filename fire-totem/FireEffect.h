#ifndef FIRE_EFFECT_H
#define FIRE_EFFECT_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "TotemDefaults.h"

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
};

class FireEffect {
public:
    // Constructors: support both pointer & reference forms seen in .ino
    FireEffect(Adafruit_NeoPixel &strip, int width = 16, int height = 8);
    FireEffect(Adafruit_NeoPixel *strip, int width = 16, int height = 8)
        : FireEffect(*strip, width, height) {}

    void begin();
    void update(float energy);
    // Back-compat: ignore dx/dy so existing .ino compiles
    void update(float energy, float /*dx*/, float /*dy*/) { update(energy); }

    void show();
    void render(); // public because .ino calls it
    void restoreDefaults();

    // For VU meter / external mapping
    uint16_t xyToIndex(int x, int y) const;

    // Make params public so SerialConsole can read/write them
    FireParams params;

private:
    Adafruit_NeoPixel &leds;
    int WIDTH;
    int HEIGHT;

    // Retro fire heat grid
    // [x][y], y=0 is top row for rendering; we still treat bottom (y=HEIGHT-1) as the "fire source".
    // We’ll map carefully in render().
    float *heat; // flat allocation: WIDTH*HEIGHT

    inline float& H(int x, int y) { return heat[y * WIDTH + x]; }
    inline const float& Hc(int x, int y) const { return heat[y * WIDTH + x]; }

    void coolCells();
    void propagateUp();
    void injectSparks(float energy);

    // palette
    uint32_t heatToColorRGB(float h) const;
};

#endif
