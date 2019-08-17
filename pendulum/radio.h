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

    void incrementMutations() {
      if (mutations < 65535) {
         this->mutations++;
      } else {
        this->mutations = 0;
      }
    }
    
    void send(pattern patternValues) {
    // Sample message send code
      byte data[7];
      data[0] = (byte) mutations;
      data[1] = (byte) mutations >> 8;
      data[2] = (byte) patternValues.color;
      data[3] = (byte) patternValues.phase;
      data[4] = (byte) patternValues.phase >> 8;
      data[5] = (byte) patternValues.phase >> 16;
      data[6] = (byte) patternValues.phase >> 24;
      
      nrf24.send(data, sizeof(data));
      nrf24.waitPacketSent();
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
            unsigned int mutationsCheck = (unsigned int)(buf[1] << 8) | buf[0];

            pattern newPattern;
            newPattern.color = buf[2];
            newPattern.phase = (unsigned long)(buf[6] << 24) | (buf[5] << 16) | (buf[4] << 8) | buf[3];

            if (mutationsCheck > this->mutations) {
              this->mutations = mutationsCheck;
              newMessage = true;
              Serial.println(String(mutationsCheck) + " " + String(newPattern.color) + " " + String(newPattern.phase));
            }
            this->newPattern = newPattern;
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
