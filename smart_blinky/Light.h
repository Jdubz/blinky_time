#ifndef Light_h
#define Light_h

#include "Color.h"

class Light {
  public:
    Light(const int Rpin, const int Gpin, const int Bpin);
    void update();
    void on();
    void off();
    void toggle();
    void changeColor(color RGB);
    void setBrightness(byte brightness);

    char* getState();
  
  private:
    bool _shouldFade();
    int _getNextColor(byte now, byte end);
    void _showColor(color RGB);
    struct color _getTargetColor();

    bool _isOn;

    int _rpin;
    int _gpin;
    int _bpin;

    byte _brightness;
    struct color _RGB;
    struct color _RGBNow;
    char _stateBuffer[256] = {0};
    unsigned long _lastFade;
};

#endif