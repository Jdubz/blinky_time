#ifndef Button_h
#define Button_h

class Button {
  public:
    Button(const int pin);
    bool isShortPress();
    bool isLongPress();
    void read();
    
  private:
    int _pin;
    bool _lastState;
    unsigned long _downPress;
    bool _shortPress;
    bool _longPress;
};

#endif
