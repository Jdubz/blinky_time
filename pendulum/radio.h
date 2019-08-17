#ifndef Radio_h
#define Radio_h

#include <SPI.h>
#include <RH_NRF24.h>

#include "Pattern.h"

RH_NRF24 nrf24;

const int CEPIN = 9;
const int CSNPIN = 10;

class Radio {
  public:
    Radio() {
      mutations = 0;
    }
    void init() {
      if (!nrf24.init()) {
        Serial.println("init failed");
      }
      // Defaults after init are 2.402 GHz (channel 2), 2Mbps, 0dBm
      if (!nrf24.setChannel(1)) {
        Serial.println("setChannel failed");
      }
      if (!nrf24.setRF(RH_NRF24::DataRate2Mbps, RH_NRF24::TransmitPower0dBm)) {
        Serial.println("setRF failed"); 
      }
    }

    pattern getNewPattern() {
      return this->newPattern;
    }
    
    void send(pattern patternValues) {
    // Sample message send code
      uint8_t data[] = "And hello back to you";
      nrf24.send(data, sizeof(data));
      nrf24.waitPacketSent();
      Serial.println("Sent a reply");
    }
    bool listen(int delayTime) {
      unsigned long start = millis();
      bool newMessage = false;
      while ((millis() - start) < delayTime) {
        // Sample message read code
        if (nrf24.available()) {
          // Should be a message for us now   
          uint8_t buf[RH_NRF24_MAX_MESSAGE_LEN];
          uint8_t len = sizeof(buf);
          
          if (nrf24.recv(buf, &len)) {
            Serial.print("got request: ");
            Serial.println((char*)buf);
            newMessage = true;
            // decode pattern values
            // this->newPattern = buf;
          } else {
            Serial.println("recv failed");
          }
        }
      }
      return newMessage;
    }
  private:
    pattern newPattern;
    unsigned int mutations;
};

#endif
