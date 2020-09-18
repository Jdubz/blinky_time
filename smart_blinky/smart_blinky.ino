#include "lib/button/Button.h"
#include "lib/led/LED.h"
#include "lib/rom/ROM.h"
#include "lib/wifi/WifiController.h"
#include "lib/serial/SerialController.h"
#include "lib/mqtt/MQTT.h"
#include "Color.h"

const int ButtonPin = D1;
const int LEDPin = D4;
const int Rpin = D5;
const int Gpin = D6;
const int Bpin = D7;

Button* button;
LED* led;
ROM* rom;
Light* light;
WifiController* wifi;
SerialController* serial;
MQTT* mqtt;

void setupWifi() {
  String SSID = rom->getSSID();
  String PW = rom->getPW();

  bool hasCreds = SSID.length() > 0 || PW.length() > 0;
  if (hasCreds) {
    wifi->setup(rom);
    if (wifi->connect()) {
      mqtt->connect();
    }
  } else {
    Serial.println("No WiFi Credentials");
  }
}

void setupLight() {
  color initColor = rom->getColor();
  if (!(initColor.R > 0 || initColor.G > 0 || initColor.B > 0)) {
    initColor.R = 150;
    initColor.G = 100;
    initColor.B = 50;
  }
  light->changeColor(initColor);

  byte brightness = rom->getBrightness();
  if (!brightness) {
    brightness = 200;
    rom->writeBrightness(brightness);
  }
  light->setBrightness(brightness);

  bool isOn = rom->getState();
  if (isOn) {
    light->on();
  }
}

void setup() {
  button = new Button(ButtonPin);
  led = new LED(LEDPin);
  rom = new ROM();
  light = new Light(Rpin, Gpin, Bpin);
  wifi = new WifiController(led);
  serial = new SerialController(rom, wifi); 
  mqtt = new MQTT(light, rom);

  setupWifi();
  setupLight();
}

void loop() {

  button->read();
  if (button->isLongPress()) {
    ESP.restart();
  }
  if (button->isShortPress()) {
    light->toggle();
  }
  light->update();

  if (serial->read()) {
    setupWifi();
  }

  if (wifi->checkConnection() && mqtt->checkConnection()) {
    mqtt->listen();
  }
}
