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

void OnDataSent(uint8_t *mac_addr, uint8_t sendStatus) {
  Serial.print("Last Packet Send Status: ");
  if (sendStatus == 0){
    Serial.println("Delivery success");
  }
  else{
    Serial.println("Delivery fail");
  }
}

class WifiServer {
  public:
    WifiServer() {}
    void startEsp() {
      WiFi.mode(WIFI_STA);

      if (esp_now_init() != 0) {
        Serial.println("Error initializing ESP-NOW");
        return;
      }
  
      Serial.println(WiFi.macAddress());

      // Once ESPNow is successfully Init, we will register for Send CB to
      // get the status of Trasnmitted packet
      esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
      esp_now_register_send_cb(OnDataSent);
      
      // Register peer
      esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
    }

    void send(char event[], float micLvl) {
      strcpy(this->data.event, event);
      this->data.timeStamp = millis();
      this->data.micLvl = micLvl;

      // Send message via ESP-NOW
      esp_now_send(broadcastAddress, (uint8_t *) &this->data, sizeof(this->data));
    }

    private:
      struct_message data;
};

#endif
