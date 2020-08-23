#include "Arduino.h"
#include "MQTT.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#define MQTT_VERSION MQTT_VERSION_3_1_1

#include "Color.h"
#include "Light.h"
#include "ROM.h"

#include "MQTTVars.h"

MQTT::MQTT(Light* light, ROM* rom) {
  WiFiClient _wifi;
  PubSubClient _client(_wifi);
  _light = light;

  _MSG_BUFFER_SIZE = 20;
  _msgBuffer = new char[_MSG_BUFFER_SIZE];

  _client.setServer(MQTT_SERVER_IP, MQTT_SERVER_PORT);
  _client.setCallback([this] (char* topic, byte* payload, unsigned int length) {
    this->_handleMessage(topic, payload, length);
  });

  Serial.print("MQTT state: ");
  Serial.println(_client.state());
}

bool MQTT::connect() {
  int maxRetries = 20;
  int retryDelay = 500;
  int retryCount = 0;
  Serial.print("Connecting to MQTT broker.");
  while (!_tryConnection() && retryCount < maxRetries) {
    delay(retryDelay);
    retryCount++;
    Serial.print(".");
  }
  return _client.connected();
}

void MQTT::listen() {
  _client.loop();
}

void MQTT::publishColor(color RGB) {
  snprintf(_msgBuffer, _MSG_BUFFER_SIZE, "%d,%d,%d", RGB.R, RGB.G, RGB.B);
  _client.publish(MQTT_LIGHT_RGB_STATE_TOPIC, _msgBuffer, true);
}

void MQTT::publishState(bool state) {
  if (state) {
    _client.publish(MQTT_LIGHT_STATE_TOPIC, LIGHT_ON, true);
  } else {
    _client.publish(MQTT_LIGHT_STATE_TOPIC, LIGHT_OFF, true);
  }
}

void MQTT::publishBrightness(byte brightness) {
  snprintf(_msgBuffer, _MSG_BUFFER_SIZE, "%d", brightness);
  _client.publish(MQTT_LIGHT_BRIGHTNESS_STATE_TOPIC, _msgBuffer, true);
}

void MQTT::_handleMessage(char* p_topic, byte* p_payload, unsigned int p_length) {
  String payload;
  for (uint8_t i = 0; i < p_length; i++) {
    payload.concat((char)p_payload[i]);
  }

  Serial.print("MQTT message received, topic: ");
  Serial.print(p_topic);
  Serial.print(" payload: ");
  Serial.println(payload);

  if (String(MQTT_LIGHT_COMMAND_TOPIC).equals(p_topic)) {
    if (payload.equals(String(LIGHT_ON))) {
      _light->on();
      _rom->writeState(true);
      publishState(true);
    } else if (payload.equals(String(LIGHT_OFF))) {
      _light->off();
      _rom->writeState(false);
      publishState(false);
    }
  } else if (String(MQTT_LIGHT_BRIGHTNESS_COMMAND_TOPIC).equals(p_topic)) {
    uint8_t brightness = payload.toInt();
    if (brightness < 0 || brightness > 255) {
      // do nothing...
      return;
    } else {
      _light->setBrightness(brightness);
      _rom->writeBrightness(brightness);
      publishBrightness(brightness);
    }
  } else if (String(MQTT_LIGHT_RGB_COMMAND_TOPIC).equals(p_topic)) {
    uint8_t firstIndex = payload.indexOf(',');
    uint8_t lastIndex = payload.lastIndexOf(',');

    color newColor;
    newColor.R = payload.substring(0, firstIndex).toInt();
    newColor.R = newColor.R = payload.substring(0, firstIndex).toInt();
    newColor.R = payload.substring(lastIndex + 1).toInt();

    _light->changeColor(newColor);
    _rom->writeColor(newColor);
    publishColor(newColor);
  }
}

bool MQTT::checkConnection() {
  if (_wifi.connected() && !_client.connected()) {
    Serial.println("Attempting MQTT connection...");

    if (!_tryConnection()) {
      Serial.print("ERROR: MQTT connection failed, rc=");
      Serial.println(_client.state());
    }
  }
  return _client.connected();
}

bool MQTT::_tryConnection() {
  if (_client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
    Serial.println("MQTT connected");

    publishState(_light->getState());
    publishBrightness(_light->getBrightness());
    publishColor(_light->getColor());

    _client.subscribe(MQTT_LIGHT_COMMAND_TOPIC);
    _client.subscribe(MQTT_LIGHT_BRIGHTNESS_COMMAND_TOPIC);
    _client.subscribe(MQTT_LIGHT_RGB_COMMAND_TOPIC);

    return true;
  }
  return false;
}
