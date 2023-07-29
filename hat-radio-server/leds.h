#ifndef Leds_h
#define Leds_h

#include <Adafruit_NeoPixel.h>

#include "types.h"
const int frameRate = 30;

class Leds {
  public:
    Leds(int LedPin, int numberLeds) {
      strip = Adafruit_NeoPixel(numLeds, LedPin, NEO_GRB + NEO_KHZ800);
      numLeds = numberLeds;
    }
    void render(color frame[]) {
      for (int led = 0; led < this->numLeds; led++) {
        this->strip.setPixelColor(led, frame[led].green, frame[led].red, frame[led].blue);
      }
      this->strip.show();
      this->clear();
      delay(1000/frameRate);
    }
    void clear() {
      for (int led = 0; led < this->numLeds; led++) {
        this->strip.setPixelColor(led, 0, 0, 0);
      }
    }
    void startup() {
      this->strip.begin();
      for (int led = 0; led < this->numLeds; led++) {
        this->strip.setPixelColor(led, 0, 50, 0);
      }
      this->strip.show();
      delay(500);
      for (int led = 0; led < this->numLeds; led++) {
        this->strip.setPixelColor(led, 50, 0, 0);
      }
      this->strip.show();
      delay(500);
      for (int led = 0; led < this->numLeds; led++) {
        this->strip.setPixelColor(led, 0, 0, 50);
      }
      this->strip.show();
      delay(500);
      this->clear();
      this->strip.show();
      delay(500);
    }

  private:
    Adafruit_NeoPixel strip;
    int numLeds;

};





#endif