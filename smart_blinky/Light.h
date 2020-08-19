#ifndef Light_h
#define Light_h

#include "Color.h"

class Light {
  public:
    Light(int Rpin, int Gpin, int Bpin);
    void changeColor(color RGB);
    color getColor();
    void update();
    bool status();

    void on();
    void off();
    void toggle();
  
  private:
    bool _shouldFade();
    byte _getNextColor(byte now, byte end);
    void _showColor(color RGB);

    bool _isOn;

    int _rpin;
    int _gpin;
    int _bpin;

    color _RGB;
    color _RGBNow;
}

#endif