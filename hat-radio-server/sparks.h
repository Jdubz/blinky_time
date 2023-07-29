
#include "chase.h"
#include "types.h"

class Sparks: public Chase {
  public:
    Sparks() {}
    void run(color frame[], float micLvl) {
      for (color pixel : frame) {
        if (random(100) * micLvl > 50) {
          pixel.green = 200;
          pixel.red = 200;
        }
        pixel.blue = 0;
        if (pixel.red > 0) {
          pixel.red -= 1;
        }
        if (pixel.green > 0) {
          pixel.green -= 2;
        }
      }
    }
};
