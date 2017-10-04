#ifndef AlternatingMode_h
#define AlternatingMode_h

#include <Adafruit_NeoPixel.h>
#include "Mode.h"
#include "Utils.h"

class AlternatingMode: public Mode {
  public:
    AlternatingMode(Adafruit_NeoPixel *strip, int* ledCount) {
      this->strip = strip;
      this->ledCount = ledCount;
    }
    void run() {
      color colorValue = getSingleColorValue();
      color flippedValue = getFlippedColorOf(colorValue);
    
      // All colors divided by two to reduce brightness and
      // preserve battery
      for (int ledIndex = 0; ledIndex < this->ledCount; ledIndex++) {
        if (ledIndex % 2 == 0) {
          this->strip->setPixelColor(ledIndex,
            float(colorValue.green / 2),
            float(colorValue.red / 2),
            float(colorValue.blue / 2));
        } else {
          this->strip->setPixelColor(ledIndex,
            float(flippedValue.green / 2),
            float(flippedValue.red / 2),
            float(flippedValue.blue / 2));
        }
      }
    }
  private:
    Adafruit_NeoPixel *strip;
    int ledCount;
};

#endif
