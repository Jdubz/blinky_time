#include <Arduino.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "MQTTController.h"

#define MQTT_VERSION MQTT_VERSION_3_1_1

#include "Color.h"
#include "Light.h"
#include "ROM.h"
#include "config.h"

MQTTController::MQTTController(PubSubClient client, Light* light, ROM* rom) {
  _client = client;
  _light = light;
  _rom = rom;

  _initTopics();
}

void MQTTController::_initTopics() {
  CLIENT_ID[7] = {0};
  CONFIG_TOPIC[sizeof(MQTT_HOME_ASSISTANT_DISCOVERY_PREFIX) + sizeof(CLIENT_ID) + sizeof(MQTT_CONFIG_TOPIC_TEMPLATE) - 4] = {0};
  STATE_TOPIC[sizeof(CLIENT_ID) + sizeof(MQTT_STATE_TOPIC_TEMPLATE) - 2] = {0};
  COMMAND_TOPIC[sizeof(CLIENT_ID) + sizeof(MQTT_COMMAND_TOPIC_TEMPLATE) - 2] = {0};
  STATUS_TOPIC[sizeof(CLIENT_ID) + sizeof(MQTT_STATUS_TOPIC_TEMPLATE) - 2] = {0};

  sprintf(CLIENT_ID, "%06X", ESP.getChipId());
  sprintf(CONFIG_TOPIC, MQTT_CONFIG_TOPIC_TEMPLATE, MQTT_HOME_ASSISTANT_DISCOVERY_PREFIX, CLIENT_ID);
  sprintf(STATE_TOPIC, MQTT_STATE_TOPIC_TEMPLATE, CLIENT_ID);
  sprintf(COMMAND_TOPIC, MQTT_COMMAND_TOPIC_TEMPLATE, CLIENT_ID);
  sprintf(STATUS_TOPIC, MQTT_STATUS_TOPIC_TEMPLATE, CLIENT_ID);
}

void MQTTController::_publish(char* topic, char* payload) {
  if (_client.publish(topic, payload, true)) {
    Serial.print("MQTT message published successfully, Topic: ");
    Serial.println(topic);
  } else {
    Serial.print("ERROR: MQTT message not published, Topic: ");
    Serial.println(topic);
  }
}

void MQTTController::handleMessage(char* topic, byte* p_payload, unsigned int length) {
  String payload;
  for (uint8_t i = 0; i < length; i++) {
    payload.concat((char)p_payload[i]);
  }

  Serial.print("MQTT message received, topic: ");
  Serial.print(topic);
  Serial.print(" payload: ");
  Serial.println(payload);

  if (String(COMMAND_TOPIC).equals(topic)) {
    DynamicJsonBuffer dynamicJsonBuffer;
    JsonObject& root = dynamicJsonBuffer.parseObject(p_payload);
    if (!root.success()) {
      Serial.print("ERROR: parseObject() failed.");
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
        _publish(STATE_TOPIC, _light->getState());
      }
    }

    if (root.containsKey("color")) {
      color newColor;
      newColor.R = root["color"]["r"];
      newColor.G = root["color"]["g"];
      newColor.B = root["color"]["b"];

      _light->changeColor(newColor);
      _rom->writeColor(newColor);
      _publish(STATE_TOPIC, _light->getState());
    }

    if (root.containsKey("brightness")) {
      uint8_t brightness = root["brightness"];

      if (brightness < 0 || brightness > 255) {
        // do nothing...
        return;
      } else {
        _light->setBrightness(brightness);
        _rom->writeBrightness(brightness);
        _publish(STATE_TOPIC, _light->getState());
      }
    }
  }
}

char* MQTTController::_getConfig() {
  JsonObject& root = _staticJsonBuffer.createObject();
  root["name"] = MQTT_ID + String(ESP.getChipId());
  root["platform"] = "mqtt_json";
  root["state_topic"] = STATE_TOPIC;
  root["command_topic"] = COMMAND_TOPIC;
  root["brightness"] = true;
  root["rgb"] = true;
  root.printTo(_jsonBuffer, sizeof(_jsonBuffer));
  return _jsonBuffer;
}

void MQTTController::startConnection() {
  _publish(STATUS_TOPIC, "alive");
  _publish(CONFIG_TOPIC, _getConfig());
  _publish(STATE_TOPIC, _light->getState());

  _client.subscribe(COMMAND_TOPIC);
}
