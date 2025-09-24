#ifndef StarsMode_h
#define StarsMode_h

#include <Adafruit_NeoPixel.h>
#include "Mode.h"
#include "Utils.h"

class StarsMode: public Mode {
  public:
    StarsMode(Adafruit_NeoPixel *strip, int* ledCount) {
      this->strip = strip;
      this->ledCount = ledCount;
      this->frame = 0;
    }
    void run() {
      float micLevel = getMicLevel();
      color colorValue = getSingleColorValue();

      for (int star = 0; star < this->ledCount; star++) {
        if (this->ledLvls[star][0] > 0) {
          if (this->ledLvls[star][1] == 1) {
            if (this->ledLvls[star][0] < 100) {
              this->ledLvls[star][0]++;
            } else {
              this->ledLvls[star][1] = 0;
            }
          } else {
            this->ledLvls[star][0]--;
          }
          float starLvl = this->ledLvls[star][0] / 100.0;
          this->strip->setPixelColor(star,
            float(colorValue.green) * micLevel * starLvl,
            float(colorValue.red) * micLevel * starLvl,
            float(colorValue.blue) * micLevel * starLvl);
        }
      }
      if (this->frame == 5) {
        int newStar = random(50);
        int newStar2 = random(50);
        this->ledLvls[newStar][0] = 1;
        this->ledLvls[newStar][1] = 1;
        this->ledLvls[newStar2][0] = 1;
        this->ledLvls[newStar2][1] = 1;
        this->frame = 0;
      }
      this->frame++;
    }
  private:
    Adafruit_NeoPixel *strip;
    int frame;
    int ledLvls[50][2];
    int* ledCount;
};

#endif
