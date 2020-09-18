#include "Arduino.h"
#include "MQTT.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define MQTT_VERSION MQTT_VERSION_3_1_1

#include "../../Color.h"
#include "../light/Light.h"
#include "../rom/ROM.h"
#include "../../config.h"

MQTT::MQTT(Light* light, ROM* rom) {
  WiFiClient _wifi;
  PubSubClient _client(_wifi);
  _light = light;
  _rom = rom;

  _initTopics();

  _client.setServer(MQTT_SERVER, MQTT_SERVER_PORT);
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

void _initTopics() {
  sprintf(CLIENT_ID, "%06X", ESP.getChipId());
  sprintf(CONFIG_TOPIC, MQTT_CONFIG_TOPIC_TEMPLATE, MQTT_HOME_ASSISTANT_DISCOVERY_PREFIX, CLIENT_ID);
  sprintf(STATE_TOPIC, MQTT_STATE_TOPIC_TEMPLATE, CLIENT_ID);
  sprintf(COMMAND_TOPIC, MQTT_COMMAND_TOPIC_TEMPLATE, CLIENT_ID);
  sprintf(STATUS_TOPIC, MQTT_STATUS_TOPIC_TEMPLATE, CLIENT_ID);
}

void MQTT::_publish(char* p_topic, char* p_payload) {
  if (mqttClient.publish(p_topic, p_payload, true)) {
    Serial.print("MQTT message published successfully, Topic: ");
    Serial.println(p_topic);
  } else {
    Serial.print("ERROR: MQTT message not published, Topic: ");
    Serial.println(p_topic);
  }
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

  if (String(COMMAND_TOPIC).equals(p_topic)) {
    DynamicJsonBuffer dynamicJsonBuffer(1024);
    JsonObject root = dynamicJsonBuffer.parseObject(p_payload);
    if (!root.success()) {
      DEBUG_PRINTLN(F("ERROR: parseObject() failed"));
      return;
    }

    if (root.containsKey("state")) {
      if (strcmp(root["state"], MQTT_STATE_ON_PAYLOAD) == 0) {
        _light->on();
        _rom->writeState(true);
        _publish(STATE_TOPIC, MQTT_STATE_ON_PAYLOAD);
      } else if (strcmp(root["state"], MQTT_STATE_OFF_PAYLOAD) == 0) {
        _light->off();
        _rom->writeState(false);
        _publish(STATE_TOPIC, MQTT_STATE_OFF_PAYLOAD);
      }
    }

    if (root.containsKey("color")) {
      color newColor;
      newColor.R = root["color"]["r"];
      newColor.G = root["color"]["g"];
      newColor.B = root["color"]["b"];

      _light->changeColor(newColor);
      _rom->writeColor(newColor);
      // FIXME
      _publish(newColor);
    }

    if (root.containsKey("brightness")) {
      uint8_t brightness = root["brightness"].toInt();
      if (brightness < 0 || brightness > 255) {
        // do nothing...
        return;
      } else {
        _light->setBrightness(brightness);
        _rom->writeBrightness(brightness);
        //FIXME
        _publish(brightness);
      }
    }
}

bool MQTT::checkConnection() {
  if (WiFi.status() != WL_CONNECTED && !_client.connected()) {
    Serial.println("Attempting MQTT connection...");

    if (!_tryConnection()) {
      Serial.print("ERROR: MQTT connection failed, rc=");
      Serial.println(_client.state());
    }
  }
  return _client.connected();
}

bool MQTT::_tryConnection() {
  if (_client.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD, )) {
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

void MQTT::
