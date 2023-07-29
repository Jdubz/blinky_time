#ifndef KeepAlive_h
#define KeepAlive_h

class KeepAlive {
  public:
    KeepAlive(int pullPin) {
      pin = pullPin;
      isLow = false;
      pinMode(pullPin, OUTPUT);
    }
    void start() {
      digitalWrite(this->pin, LOW);
      delay(100);
      digitalWrite(this->pin, HIGH);
    }
    void pullKey() {
      int pingFreq = 10;
      int now = millis();
      int seconds = now/1000;
      if (!(seconds % pingFreq) && !isLow) {
        digitalWrite(this->pin, LOW);
        this->isLow = true;
      } else if ((seconds % pingFreq) && isLow) {
        digitalWrite(this->pin, HIGH);
        this->isLow = false;
      }
    }

    private:
      bool isLow;
      int pin;
};




#endif