#ifndef KeepAlive_h
#define KeepAlive_h

class KeepAlive {
  public:
    KeepAlive(int pullPin) {
      pin = pullPin;
      isLow = false;
      pinMode(pullPin, OUTPUT);
      digitalWrite(this->pin, HIGH);
      lastPull = millis();
    }
    void pullKey(bool pull) {
      int now = millis();
      if (!isLow && pull) {
        digitalWrite(this->pin, LOW);
        this->isLow = true;
        this->lastPull = now;
      } else if (isLow && !pull && now - this->lastPull > 100) {
        digitalWrite(this->pin, HIGH);
        this->isLow = false;
      }
    }

    private:
      bool isLow;
      int pin;
      int lastPull;
};




#endif
