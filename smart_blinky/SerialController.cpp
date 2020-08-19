#include "Arduino.h"
#include "SerialController.h"

#include "ROM.h"

const int baudRate = 9600;

SerialController::SerialController(ROM rom) {
  _rom = rom;

  Serial.begin(buaudRate);
}

bool read() {
  if (Serial.available()) {
    string message = Serial.readString();
    string msgType = message.substring(0, message.indexOf(':'));
    if (msgType == 'wificreds') {
      string SSID = message.substring(message.indexOf(':') + 1, message.lastIndexOf(':'));
      Serial.println(SSID);
      _rom.writeSSID(SSID);
      string PW = message.substring(message.lastIndexOf(':') + 1);
      Serial.println(PW);
      _rom.writePW(PW);
      return true;
    }
  }
  return false;
}