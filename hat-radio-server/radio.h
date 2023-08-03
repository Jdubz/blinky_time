// /*
//   SensorServer
  
//   Basic EspNowConnection Server implementation for a sensor server (gateway)

//   HOWTO Arduino IDE:
//   - Prepare two ESP8266 or ESP32 based devices (eg. WeMos) or mix an ESP8266 and an ESP32 device
//   - Start two separate instances of Arduino IDE and load 
//     on the first one the 'SensorServer.ino' and
//     on the second one the 'SensorClientDigitalInput.ino' sketch and upload 
//     these to the two ESP devices.
//   - Start the 'Serial Monitor' in both instances and set baud rate to 9600
//   - Type 'startpair' into the edit box of the SensorServer. Hold the pairing button on the SensorClientDigitalInput device and reset the device
//   - After server and client are paired, you can change the sleep time of the client in the server
//     by typing 'settimeout <seconds>' into the serial terminal. 
//     This will than be send next time when sensor is up.
  
//   - You can use multiple clients which can be connected to one server

//   Created 11 Mai 2020
//   By Erich O. Pintar
//   Modified 06 Oct 2020
//   By Erich O. Pintar

//   https://github.com/saghonfly/SimpleEspNowConnection

// */

#ifndef Radio_h
#define Radio_h

#include "SimpleEspNowConnection.h"

static SimpleEspNowConnection simpleEspConnection(SimpleEspNowRole::SERVER);

class Server {
  public:
    Server() {}
    void OnSendError(uint8_t* ad) {
      Serial.println("Sending to '"+simpleEspConnection.macToStr(ad)+"' was not possible!");  
    }
    void OnSendError(uint8_t* ad) {
      Serial.println("Sending to '"+simpleEspConnection.macToStr(ad)+"' was not possible!");  
    }
    void OnMessage(uint8_t* ad, const uint8_t* message, size_t len) {
      Serial.println("Client '"+simpleEspConnection.macToStr(ad)+"' has sent me '"+String((char *)message)+"'");
    }
    void OnPaired(uint8_t *ga, String ad) {
      Serial.println("EspNowConnection : Client '"+ad+"' paired! ");
      simpleEspConnection.endPairing();  
    }
    void OnConnected(uint8_t *ga, String ad) {
      Serial.println("connected: "+ad);
    }
    void startEsp() {
      simpleEspConnection.begin();
    //  simpleEspConnection.setPairingBlinkPort(2);
      simpleEspConnection.onMessage(this->OnMessage);  
      simpleEspConnection.onPaired(this->OnPaired);  
      simpleEspConnection.onSendError(this->OnSendError);  
      simpleEspConnection.onConnected(this->OnConnected);  
    }
    
    void espLoop() {
      simpleEspConnection.loop();
    }
    // simpleEspConnection.startPairing(120);
}










  
  while (Serial.available()) 
  {
    char inChar = (char)Serial.read();
    if (inChar == '\n') 
    {
      Serial.println(inputString);

      if(inputString == "startpair")
      {
        Serial.println("Pairing started...");
        
      }
      else if(inputString.substring(0,11) == "settimeout ")
      {
        Serial.println("Will set new timeout of client next time when the device goes up...");
        newTimeout = atoi(inputString.substring(11).c_str());
      }          
            
      inputString = "";
    }
  }  
}

#endif