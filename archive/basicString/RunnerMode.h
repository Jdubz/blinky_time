#ifndef RunnerMode_h
#define RunnerMode_h

#include <Adafruit_NeoPixel.h>
#include "Mode.h"
#include "Utils.h"

class Runner {
  public:
    Runner(Adafruit_NeoPixel *strip) {
      this->strip = strip;
      this->headIndex = 0;
      this->tailLength = 5;
      this->runThrottle = 15;
      this->runCallCount = 0;
      this->totalPixels = strip->numPixels();
    }
    void run() {
      this->handleThrottling();

      int lastIndex = this->headIndex;

      color currentColor = getSingleColorValue();

      for (int count = 0; count < this->tailLength; count++) {
        float diminish = float(this->tailLength - count)/float(this->tailLength);
        this->strip->setPixelColor(this->headIndex - count,
          currentColor.green * diminish,
          currentColor.red * diminish,
          currentColor.blue * diminish);
      }
    }
    void setHeadIndex(int newHeadIndex) {
      this->headIndex = newHeadIndex;
    }
    int getHeadIndex() {
      return this->headIndex;
    }
  private:
    void handleThrottling() {
      this->runCallCount++;

      if (this->runCallCount >= this->runThrottle) {
        this->headIndex++;

        // reset to 0 if out of bounds
        if (this->headIndex > this->totalPixels) {
          this->headIndex = 0;
        }

        this->runCallCount = 0;
      }
    }
    Adafruit_NeoPixel *strip;
    int headIndex;
    int tailLength;
    // determines throttle, lower is faster, should probably change this
    int runThrottle;
    int runCallCount;
    int totalPixels;
};

class RunnerMode: public Mode {
  public:
    RunnerMode(Adafruit_NeoPixel *strip, int ledCount) {
      int quarterOfLeds = ledCount / 4;
      this->runner1 = new Runner(strip);
      this->runner1->setHeadIndex(0);
      this->runner2 = new Runner(strip);
      this->runner2->setHeadIndex(quarterOfLeds);
      this->runner3 = new Runner(strip);
      this->runner3->setHeadIndex(quarterOfLeds * 2);
      this->runner4 = new Runner(strip);
      this->runner4->setHeadIndex(quarterOfLeds * 3);
    }
    void run() {
      this->runner1->run();
      this->runner2->run();
      this->runner3->run();
      this->runner4->run();
    }
  private:
    Runner *runner1;
    Runner *runner2;
    Runner *runner3;
    Runner *runner4;
};

#endif
