#ifndef LED_h
#define LED_h

class LED {
  public:
    LED(const int pin);
    void on();
    void off();
    void toggle();

  private:
    int _pin;
    bool _isOn;
};

#endif
