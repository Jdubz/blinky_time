#ifndef Timer_h
#define Timer_h

#include <MillisTimer.h>

// TODO abstract to generic event new Timer(ms), Timer.event()

const int framerate = 30;
bool hasRendered = false;

void prime(MillisTimer &mt) {
  if (hasRendered) {
    hasRendered = false;
  }
}

class Timer {
  public:
    Timer() {
      this->renderTimer = MillisTimer(framerate);
      this->renderTimer.expiredHandler(prime);
      this->renderTimer.start();
    }
    bool render() {
      this->renderTimer.run();
      if (!hasRendered) {
        hasRendered = true;
        return true;
      }
      return false;
    }

    private:
      MillisTimer renderTimer;
};

#endif