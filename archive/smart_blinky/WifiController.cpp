#include <Arduino.h>
extern "C" {
  #include <user_interface.h>
}
#include <ESP8266WiFi.h>

#include "WifiController.h"
#include "LED.h"

WifiController::WifiController(LED* led) {
  _led = led;
}

void WifiController::setup(String SSID, String PW) {
  _SSID = SSID;
  _PW = PW;

  int ID = ESP.getChipId();
  uint8_t mac[] = {
    0x5C, 0xCF, 0x7F, ID & 0xFF, (ID >> 8) & 0xFF, (ID >> 16) & 0xFF
  };
  wifi_set_macaddr(STATION_IF, &mac[0]);
  WiFi.hostname("blinky-time-" + ID);
  Serial.println(WiFi.macAddress());
  Serial.println("Connecting to " + SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(_SSID, _PW);
}

bool WifiController::connect() {
  byte status = WiFi.waitForConnectResult();
  if (status != WL_CONNECTED) {
    Serial.printf("Connection Failed, status: %d\n", status);
    WiFi.printDiag(Serial);
    return false;
  } else {
    Serial.print("Wifi Connected: ");
    Serial.println(WiFi.localIP());
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    return true;
  }
}

bool WifiController::isConnected() {
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