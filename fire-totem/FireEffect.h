#ifndef FIRE_EFFECT_H
#define FIRE_EFFECT_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "TotemDefaults.h"

struct FireParams {
    float cooling = Defaults::Cooling;       // base cooling rate
    float sparkChance = Defaults::SparkChance;
    float sparkHeatMin = Defaults::SparkHeatMin;
    float sparkHeatMax = Defaults::SparkHeatMax;
    bool vuTopRowEnabled = Defaults::VuTopRowEnabled;
};

class FireEffect {
public:
    FireEffect(Adafruit_NeoPixel &strip);

    void begin();
    void update(float energy);
    void show();

    void restoreDefaults();

private:
    Adafruit_NeoPixel &leds;
    FireParams params;

    static const int WIDTH = 16;
    static const int HEIGHT = 8;

    float heat[WIDTH][HEIGHT];

    void coolCells();
    void propagateUp();
    void injectSparks(float energy);
    void render();
};

#endif
