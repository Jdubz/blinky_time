#include "Arduino.h"
#include "MQTT.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define MQTT_VERSION MQTT_VERSION_3_1_1

#include "Color.h"
#include "Light.h"
#include "ROM.h"
#include "config.h"

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
  Serial.println(_client.connected());
  return _client.connected();
}

void MQTT::listen() {
  _client.loop();
}

void MQTT::_initTopics() {
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

void MQTT::_publish(char* topic, char* payload) {
  if (_client.publish(topic, payload, true)) {
    Serial.print("MQTT message published successfully, Topic: ");
    Serial.println(topic);
  } else {
    Serial.print("ERROR: MQTT message not published, Topic: ");
    Serial.println(topic);
  }
}

void MQTT::_handleMessage(char* topic, byte* p_payload, unsigned int length) {
  String payload;
  for (uint8_t i = 0; i < length; i++) {
    payload.concat((char)p_payload[i]);
  }

  Serial.print("MQTT message received, topic: ");
  Serial.print(topic);
  Serial.print(" payload: ");
  Serial.println(payload);

  if (String(COMMAND_TOPIC).equals(topic)) {
    DynamicJsonDocument doc(256);
    auto error = deserializeJson(doc, payload);
    if (error) {
      Serial.print("ERROR: deserializeJson() failed: ");
      Serial.println(error.c_str());
      return;
    }

    if (doc.containsKey("state")) {
      if (strcmp(doc["state"], MQTT_STATE_ON_PAYLOAD) == 0) {
        _light->on();
        _rom->writeState(true);
        _publish(STATE_TOPIC, MQTT_STATE_ON_PAYLOAD);
      } else if (strcmp(doc["state"], MQTT_STATE_OFF_PAYLOAD) == 0) {
        _light->off();
        _rom->writeState(false);
        _publish(STATE_TOPIC, _light->getState());
      }
    }

    if (doc.containsKey("color")) {
      color newColor;
      newColor.R = doc["color"]["r"];
      newColor.G = doc["color"]["g"];
      newColor.B = doc["color"]["b"];

      _light->changeColor(newColor);
      _rom->writeColor(newColor);
      _publish(STATE_TOPIC, _light->getState());
    }

    if (doc.containsKey("brightness")) {
      uint8_t brightness = doc["brightness"];

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

bool MQTT::checkConnection() {
  if (WiFi.status() != WL_CONNECTED && !_client.connected()) {
    Serial.println("Attempting MQTT connectionlib.");

    if (!_tryConnection()) {
      Serial.print("ERROR: MQTT connection failed, rc=");
      Serial.println(_client.state());
    }
  }
  return _client.connected();
}

char* MQTT::_getConfig() {
  StaticJsonDocument<256> doc;
  JsonObject root = doc.to<JsonObject>();
  root["name"] = MQTT_ID + String(ESP.getChipId());
  root["platform"] = "mqtt_json";
  root["state_topic"] = STATE_TOPIC;
  root["command_topic"] = COMMAND_TOPIC;
  root["brightness"] = true;
  root["rgb"] = true;
  char config[256];
  serializeJson(root, config);
  return config;
}

bool MQTT::_tryConnection() {
  if (_client.connect(CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD, STATUS_TOPIC, 0, 1, "dead")) {
    Serial.println("MQTT connected");
    
    _publish(STATUS_TOPIC, "alive");
    _publish(CONFIG_TOPIC, _getConfig());
    _publish(STATE_TOPIC, _light->getState());

    _client.subscribe(COMMAND_TOPIC);

    return true;
  }
  return false;
}
