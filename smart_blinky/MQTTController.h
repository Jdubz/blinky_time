#ifndef MQTTController_h
#define MQTTController_h

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "Color.h"
#include "Light.h"
#include "ROM.h"

class MQTTController {
  public:
    MQTTController(PubSubClient client, Light* light, ROM* rom);
    void handleMessage(char* p_topic, byte* p_payload, unsigned int p_length);
    void startConnection();

    char CLIENT_ID[];
    char CONFIG_TOPIC[];
    char STATE_TOPIC[];
    char COMMAND_TOPIC[];
    char STATUS_TOPIC[];

  private:
    PubSubClient  _client;
    WiFiClient _wifi;
    Light* _light;
    ROM* _rom;

    void _initTopics();
    void _publish(char* topic, char* payload);
    char* _getConfig();

    char _jsonBuffer[256] = {0};
    StaticJsonBuffer<256> _staticJsonBuffer;
};

#endif