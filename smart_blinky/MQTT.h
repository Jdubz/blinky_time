#ifndef MQTT_h
#define MQTT_h

#include <ESP8266WiFi.h>
#include <PubSubClient.h>

#include "Color.h"
#include "Light.h"
#include "ROM.h"

class MQTT {
  public:
    MQTT(Light* light, ROM* rom);
    bool checkConnection();
    void listen();
    bool connect();

  private:
    PubSubClient  _client;
    WiFiClient _wifi;
    Light* _light;
    ROM* _rom;
    int _MSG_BUFFER_SIZE;
    char* _msgBuffer;

    void _handleMessage(char* p_topic, byte* p_payload, unsigned int p_length);
    bool _tryConnection();
    void _initTopics();
    void _publish(char* topic, char* payload);
    char* _getConfig();

    char CLIENT_ID[];
    char CONFIG_TOPIC[];
    char STATE_TOPIC[];
    char COMMAND_TOPIC[];
    char STATUS_TOPIC[];
};

#endif