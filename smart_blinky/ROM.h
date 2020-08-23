#ifndef ROM_h
#define ROM_h

#include "Color.h"

class ROM {
  public:
    ROM();
    String getSSID();
    String getPW();
    struct color getColor();
    bool getState();
    byte getBrightness();

    void writeSSID(String SSID);
    void writePW(String PW);
    void writeColor(color RGB);
    void writeState(bool state);
    void writeBrightness(byte brightness);

    void forgetCreds();

    private:
      String _readString(int address);
      void _writeString(int address, String data);
};

#endif
