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
    void publishColor(color RGB);
    void publishState(bool state);
    void publishBrightness(byte brightness);
    bool checkConnection();
    void listen();

  private:
    PubSubClient  _client;
    WiFiClient _wifi;
    Light* _light;
    ROM* _rom;
    int _MSG_BUFFER_SIZE;
    char* _msgBuffer;

    void _handleMessage(char* p_topic, byte* p_payload, unsigned int p_length);
};

#endif