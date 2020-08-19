#include "Arduino.h"
#include "WifiController.h"

#include "LED.h"

#include <ESP8266WiFi.h>

const int max_retries = 20;
const int retry_delay = 500;

WifiController::WifiController(LED led) {
  _led = led;
}

void WifiController::setup(const char* SSID, const char* PASSWORD) {
  _SSID = SSID;
  _PW = PASSWORD;
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);  
}

bool WifiController::_connect() {
  int retries = 0;
  while ((WiFi.status() != WL_CONNECTED) && (retries < max_retries)) {
    retries++;
    delay(retry_delay);
    _led.toggle();
  }
  bool connected = WiFi.status();
  if (connected) {
    Serial.println(WiFi.localIP());
  }

  return connected;
}

bool WifiController::_isConnected() {
  if (WiFi.status()) {
    _led.on();
  } else {
    _led.off();
  }
  return WiFi.status();
}

bool WifiController::checkConnection() {
  bool connected = _isConnected();
  bool hasCreds = !!_SSID && !!_PW;
  if (!connected && hasCreds) {
    return _connect();
  }
  return connected && hasCreds;
}