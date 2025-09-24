
#include "chase.h"

int fadeSpeed = 7;
int sparkBase = 20;
int newSparksBase = 10;

class PunchSparks: public Chase {
  public:
    PunchSparks(int numPixels) {
      numSparks = numPixels;
    }
    void run(color frame[], float micLvl) {
      int newSparks = 2 + newSparksBase * micLvl;
      for (int spark = 0; spark < newSparks; spark++) {
        int center = random(this->numSparks);
        int sparkSize = int(sparkBase + (255 - sparkBase) * micLvl);
        frame[center].red += sparkSize;
        frame[center].green += sparkSize;
      }
      
      for (int pixel = 0; pixel < this->numSparks; pixel++) {
        frame[pixel].blue = 0;
        if (frame[pixel].red > 255)  { frame[pixel].red = 255; }
        if (frame[pixel].red >= fadeSpeed) {
          frame[pixel].red -= fadeSpeed;
        } else { frame[pixel].red = 0; }
        frame[pixel].green = frame[pixel].red * .8;
      }
    }

  private:
    int numSparks;
};
