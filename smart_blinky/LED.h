#ifndef LED_h
#define LED_h

class LED {
  public:
    LED(int LedPin);
    void on();
    void off();
    void toggle();

  private:
    int _pin;
    bool isOn;
};

#endif
