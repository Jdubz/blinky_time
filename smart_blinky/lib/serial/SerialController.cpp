#include "Arduino.h"
#include "SerialController.h"

#include "ROM.h"
#include "WifiController.h"

SerialController::SerialController(ROM* rom, WifiController* wifi) {
  int baudRate = 115200;
  _rom = rom;
  _wifi = wifi;

  Serial.begin(baudRate);
}

bool SerialController::read() {
  if (Serial.available()) {
    String message = Serial.readString();
    String msgType = message.substring(0, message.indexOf(":"));
    if (msgType == "wificreds") {
      String SSID = message.substring(message.indexOf(":") + 1, message.lastIndexOf(':'));
      Serial.println(SSID);
      _rom->writeSSID(SSID);
      String PW = message.substring(message.lastIndexOf(":") + 1);
      Serial.println(PW);
      _rom->writePW(PW);
      return true;
    } else if (msgType == "ip") {
      Serial.println(_wifi->getIp());
    }
  }
  return false;
}