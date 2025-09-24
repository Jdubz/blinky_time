#ifndef Radio_h
#define Radio_h

#include <ESP8266WiFi.h>
#include <espnow.h>

// REPLACE WITH RECEIVER MAC Address
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Structure example to send data
// Must match the receiver structure
typedef struct struct_message {
  char event[32];
  int timeStamp;
  float micLvl;
} struct_message;

struct_message data;

void OnDataRecv(uint8_t * mac, uint8_t *incomingData, uint8_t len) {
  memcpy(&data, incomingData, sizeof(data));
}

class WifiClient {
  public:
    WifiClient() {}
    void startEsp() {
      WiFi.mode(WIFI_STA);

      if (esp_now_init() != 0) {
        Serial.println("Error initializing ESP-NOW");
        return;
      }
  
      Serial.println(WiFi.macAddress());

      // Once ESPNow is successfully Init, we will register for recv CB to
      // get recv packer info
      esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
      esp_now_register_recv_cb(OnDataRecv);
    }

    float read() {
      float micLvl = data.micLvl;
      return micLvl;
    }
};

#endif
