
#include "chase.h"

class Sparks: public Chase {
  public:
    Sparks(int numPixels) {
      numSparks = numPixels;
    }
    void run(color frame[], float micLvl) {
      for (int pixel = 0; pixel < this->numSparks; pixel++) {
        float seed = float(random(100)) + (20.0 * micLvl);
        if (seed > 80.0) {
          float brightness = float(random(200)) * micLvl;
          frame[pixel].green = int(brightness * (float(random(5)) * 0.2));
          frame[pixel].red = int(brightness);
        }
        frame[pixel].blue = 0;
        if (frame[pixel].red >= 1) {
          frame[pixel].red -= 1;
        } else { frame[pixel].red = 0; }
        if (frame[pixel].green >= 2) {
          frame[pixel].green -= 2;
        } else { frame[pixel].green = 0; }
      }
    }

  private:
    int numSparks;
};
