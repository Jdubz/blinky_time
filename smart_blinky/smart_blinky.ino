#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include "Button.h"
#include "LED.h"
#include "ROM.h"
#include "WifiController.h"
#include "SerialController.h"
#include "MQTTController.h"
#include "Color.h"
#include "config.h"

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
MQTTController* mqtt;

WiFiClient    wifiClient;
PubSubClient  mqttClient(wifiClient);

volatile unsigned long lastMQTTConnection = 0;

bool connectMQTT() {
  if (!mqttClient.connected()) {
    if (lastMQTTConnection + MQTT_CONNECTION_TIMEOUT < millis()) {
      if (mqttClient.connect(mqtt->CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD, mqtt->STATUS_TOPIC, 0, 1, "dead")) {
        Serial.println("MQTT connection Success");
        mqtt->startConnection();
        return true;
      }
      return false;
    }
    return false;
  }
  return true;
}

void setupWifi() {
  String SSID = rom->getSSID();
  String PW = rom->getPW();

  if (SSID.length() == 0) {
    SSID = DEFAULT_SSID;
    PW = DEFAULT_PW;
  }

  wifi->setup(SSID, PW);
  wifi->connect();
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
  mqtt = new MQTTController(mqttClient, light, rom);

  setupWifi();
  setupLight();
  mqttClient.setServer(MQTT_SERVER, MQTT_SERVER_PORT);
  mqttClient.setCallback([](char* p_topic, byte* p_payload, unsigned int p_length) {
    mqtt->handleMessage(p_topic, p_payload, p_length);
  });
}

void loop() {
  // Serial.println(ESP.getFreeHeap(),DEC);
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

  if (wifi->isConnected()) {
    if (connectMQTT()) {
      mqttClient.loop();
    }
  }
}
