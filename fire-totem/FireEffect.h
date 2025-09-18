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

    // --- Enhanced IMU-driven inputs ---
    void setWindScale(float colsPerSec) { windColsPerSec = colsPerSec; }  // optional

    // Basic wind interface (backwards compatible)
    void setWind(float wx, float wy) { windX = wx; windY = wy; }
    void setUpVector(float ux, float uy, float uz) { upx=ux; upy=uy; upz=uz; }
    void setStoke(float s) { stoke = (s < 0.f ? 0.f : (s > 1.f ? 1.f : s)); }

    // Enhanced torch physics interface
    void setTorchMotion(float windX, float windY, float stokeLevel,
                        float turbulence, float centrifugal, float flameBend,
                        float tiltAngle, float motionIntensity);

    void setRotationalEffects(float spinMag, float centrifugalForce);
    void setInertialDrift(float driftX, float driftY);
    void setFlameDirection(float direction, float bend);
    void setMotionTurbulence(float level);

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

    // Enhanced IMU-fed torch physics state
    float windX = 0.f, windY = 0.f;
    float upx = 0.f, upy = 1.f, upz = 0.f;  // unit "world up"
    float stoke = 0.f;

    // Advanced motion effects
    float turbulenceLevel = 0.f;     // motion-induced turbulence (0-1)
    float centrifugalEffect = 0.f;   // rotational spreading effect
    float flameBendAngle = 0.f;      // flame bending from motion
    float flameDirection = 0.f;      // direction flames lean (degrees)
    float tiltAngle = 0.f;           // torch tilt from vertical
    float motionIntensity = 0.f;     // overall motion level
    float spinMagnitude = 0.f;       // rotation speed
    float inertiaDriftX = 0.f;       // momentum-based drift
    float inertiaDriftY = 0.f;

    // Dynamic spark behavior
    float sparkHeadX = 0.f;          // wind-biased spark position
    float sparkHeadY = 0.f;          // vertical spark drift
    float sparkIntensity = 1.f;      // motion-influenced spark intensity

    // Adaptive parameters - tuned for balance between physics and visuals
    float windColsPerSec = 4.0f;     // moderate wind responsiveness
    int   sparkSpreadCols = 2;       // moderate spread for motion effects
    float motionSparkFactor = 0.8f;  // gentle motion enhancement
    float turbulenceScale = 0.8f;    // reduced turbulence influence

    float* heatScratch = nullptr;    // temp row buffer for advection
};

