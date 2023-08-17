
#include "chase.h"

class Sparks: public Chase {
  public:
    Sparks(int numPixels) {
      numSparks = numPixels;
    }
    void run(color frame[], float micLvl) {
      int newSparks = 2 + 20 * micLvl;
      for (int spark = 0; spark < newSparks; spark++) {
        int center = random(this->numSparks);
        frame[center].red += int(10 + 200 * micLvl);
        frame[center].green += int(10 + 200 * micLvl);
      }
      
      for (int pixel = 0; pixel < this->numSparks; pixel++) {
        frame[pixel].blue = 0;
        if (frame[pixel].green > frame[pixel].red) { frame[pixel].green = frame[pixel].red; }
        if (frame[pixel].red > 255)  { frame[pixel].red = 255; }
        if (frame[pixel].green > 255)  { frame[pixel].green = 255; }
        if (frame[pixel].red > 10) {
          frame[pixel].red -= 4;
        }
        if (frame[pixel].green > 10) {
          frame[pixel].green -= 5;
        } else { frame[pixel].green = 0; }
      }
    }

  private:
    int numSparks;
};
