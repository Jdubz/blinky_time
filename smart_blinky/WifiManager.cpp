#include "Arduino.h"
#include "WifiManager.h"

#include "LED.h"

#include <ESP8266WiFi.h>

const int max_retries = 20;
const int retry_delay = 500;

WifiManager::WifiManager(LED led) {
  _led = led;
}

bool WifiManager::connect(const char* SSID, const char* PASSWORD) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);

  int retries = 0;
  while ((WiFi.status() != WL_CONNECTED) && (retries < max_retries)) {
    retries++;
    delay(retry_delay);
    _led.toggle();
  }

  return this.connected();
}

bool WifiManager::connected() {
  if (WiFi.status()) {
    _led.on();
  } else {
    _led.off();
  }
  return WiFi.status();
}
