#ifndef ChristmasLightsMode_h
#define ChristmasLightsMode_h

#include <Adafruit_NeoPixel.h>
#include "Mode.h"
#include "Utils.h"

class ChristmasLightsMode: public Mode {
  public:
    ChristmasLightsMode(Adafruit_NeoPixel *strip, int* ledCount) {
      this->strip = strip;
      this->ledCount = ledCount;
      this->maxFullValue = 127;
      this->fullValue = 127;
      this->halfValue = 63;
    }
    void run() {
      this->setLightValues();
      
      for (int ledIndex = 0; ledIndex < this->ledCount; ledIndex++) {
        int colorIndex = ledIndex % 5;
        switch (colorIndex) {
          case 0: // Red
            this->strip->setPixelColor(ledIndex, 0, this->fullValue, 0);
            break;
          case 1: // Green
            this->strip->setPixelColor(ledIndex, this->fullValue, 0, 0);
            break;
          case 2: // Purple
            this->strip->setPixelColor(ledIndex, 0, this->fullValue, this->fullValue);
            break;
          case 3: // Blue
            this->strip->setPixelColor(ledIndex, 0, 0, this->fullValue);
            break;
          case 4: // Orange
            this->strip->setPixelColor(ledIndex, this->halfValue, this->fullValue, 0);
            break;
        }
      }
    }
    void setLightValues() {
//      float scalar = getKnobValue() / 1024;
//
//      this->fullValue = (int)(scalar * float(this->maxFullValue));
//      this->halfValue = (int)(float(this->fullValue) / 2);
    }
  private:
    Adafruit_NeoPixel *strip;
    int ledCount;
    int fullValue;
    int halfValue;
    int maxFullValue;
};

#endif
