#include "Button.h"
#include "LED.h"
#include "ROM.h"
#include "Routes.h"
#include "WifiController.h"
#include "SerialController.h"

#define ButtonPin D3
#define LEDPin D4
#define Rpin D5
#define Gpin D6
#define Bpin D7

Button button;
LED led;
ROM rom;
Light light;
WifiController wifi;
Routes api;
SerialController serial;


void setupWifi() {
  char SSID[64];
  char PW[64];

  bool hasCreds = rom.getSSID(SSID) && rom.getPW(PW);
  if (hasCreds)) {
    wifi.setup(SSID, PW);
    Serial.print('WiFi SSID: ');
    Serial.println(SSID);
    Serial.print('Wifi PW: ');
    Serial.println(PW);
  } else {
    Serial.printls('No WiFi Credentials');
  }
}

void setup() {
  button = new Button(ButtonPin);
  led = new LED(LEDPin);
  rom = new ROM();
  serial = new SerialController(rom);
  light = new Light(Rpin, Gpin, Bpin);
  wifi = new WifiController(led);
  api = new Routes(light, rom);

  setupWifi();
}

void loop() {

  button.read();
  if (button.isShortPress()) {
    light.toggle();
  }
  light.update();

  if (serial.read()) {
    setupWifi();
  }

  wifi.checkConnection();

  api.handleClient();
}
