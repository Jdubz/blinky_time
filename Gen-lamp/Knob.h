#ifndef Knob_h
#define Knob_h

class Knob {
  public:
    Knob(int KNOBPIN) {
      pin = KNOBPIN;
    }
    bool update() {
      int newVal = analogRead(this->pin);
      if (newVal > this->value +3 || newVal < this->value -3) {
        this->value = newVal;
        return true;
      }
      return false;
    }
    int getValue() {
      return this->value;
    }
  private:
    int pin;
    bool changed;
    int value;
};

#endif
