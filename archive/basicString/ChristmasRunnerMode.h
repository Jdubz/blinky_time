#ifndef ChristmasRunnerMode_h
#define ChristmasRunnerMode_h

#include <Adafruit_NeoPixel.h>
#include "Mode.h"
#include "Utils.h"

const int COLOR_COUNT = 5;

class ChristmasRunnerMode: public Mode {
  public:
    ChristmasRunnerMode(Adafruit_NeoPixel *strip, int* ledCount) {
      this->strip = strip;
      this->ledCount = ledCount;
      this->offset = 0;
      this->runCounter = 0;
      this->fullValue = 127;
      this->halfValue = 63;
      this->runSpeed = 30;
    }
    void run() {
      float fullValue = 255;
      float halfValue = 127;
    
      // All colors divided by two to reduce brightness and
      // preserve battery
      for (int ledIndex = 0; ledIndex < this->ledCount; ledIndex++) {
        int colorIndex = (ledIndex + this->offset) % COLOR_COUNT;
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

      this->runSpeed = 15 + (int)((float(getKnobValue()) / 1024.0f) * 40.0f);
      
      this->runCounter++;
      if (this->runCounter >= this->runSpeed) {
        this->runCounter = 0;
        this->offset = (this->offset + 1) % COLOR_COUNT;
      }
    }
  private:
    Adafruit_NeoPixel *strip;
    int ledCount;
    int offset;
    int runCounter;
    int runSpeed;
    int fullValue;
    int halfValue;
};

#endif
