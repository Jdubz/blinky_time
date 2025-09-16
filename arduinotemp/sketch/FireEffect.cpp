#line 1 "D:\\Development\\Arduino\\blinky_time\\fire-totem\\FireEffect.cpp"
#include "FireEffect.h"
#include <Adafruit_NeoPixel.h>
#include <math.h>
void FireEffect::update(float,float,float){} // stub to keep the zip self-contained
void FireEffect::render(){}; void FireEffect::restoreDefaults(){};
FireEffect::FireEffect(Adafruit_NeoPixel*, const FireParams& d): p(d) {}
int FireEffect::idx(int x, int y) const { return y*p.width + (x%p.width + p.width)%p.width; }
void FireEffect::addSparks(float){} void FireEffect::addForces(float,float,float,float){} void FireEffect::advect(float){} void FireEffect::diffuse(){} void FireEffect::cool(float,float){} uint32_t FireEffect::heatToColor(uint8_t) const { return 0; }
