#include "Arduino.h"
#include "WifiController.h"

#include "LED.h"
extern "C" {
  #include <user_interface.h>
}
#include <ESP8266WiFi.h>

WifiController::WifiController(LED* led) {
  _led = led;
}

void WifiController::setup(ROM* rom) {
  _SSID = rom->getSSID();
  _PW = rom->getPW();

  byte ID[] = rom.getID();
  byte mac[] = {
    0x5C, 0xCF, 0x7F, ID[0], ID[1], ID[2]
  };
  wifi_set_macaddr(STATION_IF, &mac[0]);
  WiFi.hostname("smarty-blink-" + String(ID));

  Serial.println("Connecting to " + SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(_SSID, _PW);
}

bool WifiController::connect() {
  int maxRetries = 20;
  int retryDelay = 500;
  int retryCount = 0;
  while (WiFi.status() != WL_CONNECTED && retryCount < maxRetries) {
    delay(retryDelay);
    retryCount++;
    Serial.print(".");
    _led->toggle();
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Connection Failed");
    return false;
  } else {
    Serial.print("Wifi Connected: ");
    Serial.println(WiFi.localIP());
    return true;
  }
}

bool WifiController::_isConnected() {
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected) {
    _led->on();
  } else {
    _led->off();
  }
  return connected;
}

String WifiController::getIp() {
  IPAddress address = WiFi.localIP();
  Serial.println(address);
  return String(address[0]) + String(".") +\
  String(address[1]) + String(".") +\
  String(address[2]) + String(".") +\
  String(address[3]);
}

bool WifiController::checkConnection() {
  bool connected = _isConnected();
  bool hasCreds = !!_SSID && !!_PW;
  if (!connected && hasCreds) {
    WiFi.reconnect();
    if (_isConnected()) {
      Serial.print("wifi connected: ");
      Serial.println(WiFi.localIP());
      return true;
    }
    return false;
  }
  return hasCreds && connected;
}