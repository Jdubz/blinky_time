#include "Arduino.h"
#include "ROM.h"

#include "EEPROM.h"
#include "Color.h"

ROM::ROM() {
  int EEPROM_SIZE = 133;
  EEPROM.begin(EEPROM_SIZE);
}

String ROM::getSSID() {
  int SSIDAddress = 0;
  return _readString(SSIDAddress);
}

String ROM::getPW() {
  int PWAddress = 64;
  return _readString(PWAddress);
}

color ROM::getColor() {
  int colorAddress = 128;

  color RGB;
  byte R = EEPROM.read(colorAddress);
  RGB.R = R;
  byte G = EEPROM.read(colorAddress + 1);
  RGB.G = G;
  byte B = EEPROM.read(colorAddress + 2);
  RGB.B = B;
  return RGB;
}

bool ROM::getState() {
  int stateAddress = 131;
  byte isOn = EEPROM.read(stateAddress);
  return !!isOn;
}

byte ROM::getBrightness() {
  int brightnessAddress = 132;
  byte brightness = EEPROM.read(brightnessAddress);
  return brightness;
}

void ROM::writeState(bool state) {
  int stateAddress = 131;
  EEPROM.write(stateAddress, state);
  EEPROM.commit();
}

void ROM::writeSSID(String SSID) {
  int SSIDAddress = 0;
  _writeString(SSIDAddress, SSID);
}

void ROM::writePW(String PW) {
  int PWAddress = 64;
  _writeString(PWAddress, PW);
}

void ROM::writeColor(color RGB) {
  int colorAddress = 128;

  EEPROM.write(colorAddress, RGB.R);
  EEPROM.write(colorAddress + 1, RGB.G);
  EEPROM.write(colorAddress + 2, RGB.B);
  EEPROM.commit();
}

void ROM::writeBrightness(byte brightness) {
  int brightnessAddress = 132;
  EEPROM.write(brightness, brightnessAddress);
  EEPROM.commit();
}

void ROM::forgetCreds() {
  for (uint8_t i = 0 ; i < 128 ; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
}

String ROM::_readString(int address) {
  char data[128];
  unsigned char k;
  int length = 0;
  k = EEPROM.read(address);
  while (k != '\0' && length < 64) {
    k = EEPROM.read(address + length);
    data[length] = k;
    length++;
  }
  data[length]='\0';
  return String(data);
}

void ROM::_writeString(int address, String data) {
  int size = data.length();
  for (int i = 0; i < size; i++) {
    EEPROM.write(address + i, data[i]);
  }
  EEPROM.write(address + size, '\0');
  EEPROM.commit();
}

