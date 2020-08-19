#ifndef Button_h
#define Button_h

#include "Arduino.h"

class Button {
  public:
    Button(int ButtonPin);
    bool isShortPress();
    bool isLongPress();
    void read();
    
  private:
    int _pin;
    bool lastState;
    unsigned long downPress;
    bool shortPress;
    bool longPress;
};

#endif
