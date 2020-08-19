#include "Arduino.h"
#include "ROM.h"

#include "EEPROM.h"
#include "Color.h"

const int EEPROM_SIZE = 132;
const int SSIDAddress = 0;
const int PWAddress = 64;
const int ColorAddress = 128;

ROM::ROM() {
  if (!EEPROM.begin(EEPROM_SIZE)) {
    Serial.println("EEPROM failed to initialize");
  }
}

bool ROM::getSSID(char * buf) {
  String stringSSID = EEPROM.readString(SSIDAddress);
  unsigned char ssidLength = stringSSID.length() + 1;
  stringSSID.toCharArray(buf, ssidLength);
  return ssidLength > 1;
}

bool ROM::getPW(char * buf) {
  String stringPW = EEPROM.readString(PWAddress);
  unsigned char pwLength = stringPW.length() + 1;
  stringPW.toCharArray(buf, pwLength);
  return pwLength > 1;
}

bool ROM::getColor(color RGB) {
  byte R = EEPROM.read(colorAddress);
  RGB.R = R;
  byte G = EEPROM.read(colorAddress + 1);
  RGB.G = G;
  byte B = EEPROM.read(colorAddress + 2);
  RGB.B = B;
  return (!!R || !!G || !!B);
}

void ROM::writeSSID(string SSID) {
  EEPROM.writeString(SSIDAddress, SSID);
  EEPROM.commit();
  char PW[64];
}

void ROM::writePW(string PW) {
  EEPROM.writeString(PWAddress, PW);
  EEPROM.commit();
  char SSID[64];
}

void ROM::writeColor(color RGB) {
      EEPROM.write(colorAddress, RGB.R);
      EEPROM.write(colorAddress + 1, RGB.G);
      EEPROM.write(colorAddress + 2, RGB.B);
      EEPROM.commit();
}

void ROM::forgetCreds() {
  for (uint8_t i = 0 ; i < 128 ; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
}

