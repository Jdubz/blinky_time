#ifndef AudioLevelsMode_h
#define AudioLevelsMode_h

#include <Adafruit_NeoPixel.h>
#include "Mode.h"
#include "Utils.h"

class AudioLevelsMode: public Mode {
  public:
    AudioLevelsMode(Adafruit_NeoPixel *strip, int* ledCount) {
      this->strip = strip;
      this->ledCount = ledCount;
    }
    void run() {
      float micLevel = getMicLevel();
      color colorValue = getSingleColorValue();

      for (int test = 0; test < this->ledCount; test++) {
        this->strip->setPixelColor(test,
          float(colorValue.green) * micLevel,
          float(colorValue.red) * micLevel,
          float(colorValue.blue) * micLevel);
      }

       keepBatteryOn(this->strip);
    }
  private:
    Adafruit_NeoPixel *strip;
    int ledCount;
};

#endif
