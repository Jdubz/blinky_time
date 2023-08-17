#ifndef Microphone_h
#define Microphone_h

class Microphone {
  public:
    Microphone(int inputPin) {
      pin = inputPin;
      max = 20.0;
    }
    void update() {
      int now = analogRead(this->pin);
      if (now > this->high) {
        this->high = now;
      }
      if (now > this->max) {
        this->max = float(now);
      }
    }
    float read() {
      int sample = this->high;
      this->high = 0;
      return float(sample) / this->max;
    }
    void attenuate() {
      float decay = 0.25;
      if (this->max >= 20.0) {
        this->max -= decay;
      }
    }

  private:
    int pin;
    int high;
    float max;

};

#endif
