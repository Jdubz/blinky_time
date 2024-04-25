#ifndef Timer_h
#define Timer_h

class Timer {
  public:
    Timer(int time) {
      rate = time;
      lastFrame = 0;
    }
    bool trigger() {
      int now = millis();
      int laps = now / this->rate;
      if (laps > this->lastFrame) {
        this->lastFrame = laps;
        return true;
      }
      return false;
    }

    private:
      int rate;
      int lastFrame;
};

#endif
