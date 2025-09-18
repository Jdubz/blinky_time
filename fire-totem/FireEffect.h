#pragma once

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
    uint8_t transientHeatMax    = Defaults::TransientHeatMax;
};

class FireEffect {
public:
    // Constructors: support both pointer & reference forms seen in .ino
    FireEffect(Adafruit_NeoPixel &strip, int width = 16, int height = 8);
    FireEffect(Adafruit_NeoPixel *strip, int width = 16, int height = 8)
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

    // Make params public so SerialConsole can read/write them
    FireParams params;

    // --- IMU-driven inputs ---
    void setWindScale(float colsPerSec) { windColsPerSec = colsPerSec; }  // optional

    void setWind(float wx, float wy) { windX = wx; windY = wy; } // wx used for columns
    void setUpVector(float ux, float uy, float uz) { upx=ux; upy=uy; upz=uz; }
    void setStoke(float s) { stoke = (s < 0.f ? 0.f : (s > 1.f ? 1.f : s)); }

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

    // palette
    uint32_t heatToColorRGB(float h) const;

    // IMU-fed state
    float windX = 0.f, windY = 0.f;
    float upx = 0.f, upy = 1.f, upz = 0.f;  // unit "world up"
    float stoke = 0.f;

    // Wind-biased “spark head” that drifts around the cylinder
    float sparkHeadX = 0.f;

    // Minimal knobs (fixed defaults; we can expose later if desired)
    float windColsPerSec = 6.0f;  // columns/sec per unit windX
    int   sparkSpreadCols = 2;    // ± columns around head where we drop sparks
    float* heatScratch = nullptr;     // temp row buffer for advection
};

