#ifndef ROM_h
#define ROM_h

#include "Color.h"

class ROM {
  public:
    ROM();
    bool getSSID(char * buf);
    bool getPW(char * buf);
    bool getColor(color RGB);

    void writeSSID(String SSID);
    void writePW(String PW);
    void writeColor(color RGB);

    void forgetCreds();
};

#endif
