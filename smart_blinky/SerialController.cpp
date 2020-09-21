#include "Arduino.h"
#include "SerialController.h"

#include "ROM.h"
#include "WifiController.h"

SerialController::SerialController(ROM* rom, WifiController* wifi) {
  int baudRate = 115200;
  _rom = rom;
  _wifi = wifi;

  Serial.begin(baudRate);
  // Serial.setDebugOutput(true);
}

bool SerialController::read() {
  if (Serial.available()) {
    String message = Serial.readString();
    Serial.println("recieved message: \"" + message + "\"");
    String msgType = message.substring(0, message.indexOf(":"));
    if (msgType == "wificreds") {
      String SSID = message.substring(message.indexOf(":") + 1, message.lastIndexOf(':'));
      _rom->writeSSID(SSID);
      String PW = message.substring(message.lastIndexOf(":") + 1);
      PW.trim();
      _rom->writePW(PW);
      return true;
    } else if (msgType == "ip") {
      Serial.println(_wifi->getIp());
    } else if (msgType == "forget") {
      _rom->forgetCreds();
      return true;
    }
  }
  return false;
}