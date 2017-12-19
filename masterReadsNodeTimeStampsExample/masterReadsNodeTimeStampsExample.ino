#include <Adafruit_NeoPixel.h>
#include "RF24.h"
#include "RF24Network.h"
#include "RF24Mesh.h"
#include <SPI.h>
//#include <printf.h>

/*
 * Configure NODE_ID as 0 for the master node and 1-255 for other nodes
 * This will be stored in EEPROM on AVR devices, so remains persistent between further uploads, loss of power, etc.
 */
#define NODE_ID 1
#if NODE_ID == 0
  #define IS_MASTER true
#endif

/*
 * Pin Constants
 */
const int LEDPIN = 2;
const int BUTTONPIN = 3;
const int KNOBPIN = 14;
const int CEPIN = 9;
const int CSNPIN = 10;

/*
 * LED Stuff
 */
const int NUM_LEDS = 5;
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

void renderStrip(Adafruit_NeoPixel* strip, int g, int r, int b) {
  // Set all LEDs to red
  for (int index = 0; index < NUM_LEDS; index++) {
    strip->setPixelColor(index, g, r, b);
  }

  strip->show();
}

void clearStrip(Adafruit_NeoPixel* strip) {
  renderStrip(strip, 0, 0, 0);
}

void setupStrip(Adafruit_NeoPixel* strip) {
  strip->begin();
  renderStrip(strip, 0, 100, 0);
  strip->show();
}

/**** Configure the nrf24l01 CE and CS pins ****/
RF24 radio(CEPIN, CSNPIN);
RF24Network network(radio);
RF24Mesh mesh(radio, network);

uint32_t displayTimer = 0;

struct payload_t {
  unsigned long ms;
  unsigned long counter;
};

void setup() {

  Serial.begin(9600);
//  printf_begin();

  setupStrip(&strip);
  
  // Set the nodeID manually
  mesh.setNodeID(NODE_ID);

  // Connect to the mesh
  Serial.println("Connecting to the mesh...");
  mesh.begin();
  Serial.println("Mesh begun");

  clearStrip(&strip);
}



void loop() {
  #if defined(IS_MASTER)
    clearStrip(&strip);

    // Call mesh.update to keep the network updated
    mesh.update();
  
    // In addition, keep the 'DHCP service' running on the master node so addresses will
    // be assigned to the sensor nodes
    mesh.DHCP();
  
  
    // Check for incoming data from the sensors
    if(network.available()){
      renderStrip(&strip, 0, 0, 100);
      RF24NetworkHeader header;
      network.peek(header);
  
      uint32_t dat=0;
      switch(header.type){
        // Display the incoming millis() values from the sensor nodes
        case 'M': 
          network.read(header,&dat,sizeof(dat)); 
          Serial.println(dat); 
          break;
        default: 
          network.read(header,0,0); 
          Serial.println(header.type);
          break;
      }
  
      clearStrip(&strip);
    }
  
    if(millis() - displayTimer > 5000){
      displayTimer = millis();
      Serial.println(" ");
      Serial.println(F("********Assigned Addresses********"));
       for(int i=0; i<mesh.addrListTop; i++){
         Serial.print("NodeID: ");
         Serial.print(mesh.addrList[i].nodeID);
         Serial.print(" RF24Network Address: 0");
         Serial.println(mesh.addrList[i].address,OCT);
       }
      Serial.println(F("**********************************"));
    }
  #else
    static unsigned long lastLightTurnedOn = 0;

    mesh.update();
  
    // Send to the master node every second
    if (millis() - displayTimer >= 1000) {
      displayTimer = millis();
  
      // Send an 'M' type message containing the current millis()
      if (!mesh.write(&displayTimer, 'M', sizeof(displayTimer))) {
  
        // If a write fails, check connectivity to the mesh network
        if ( ! mesh.checkConnection() ) {
          //refresh the network address
          renderStrip(&strip, 0, 0, 100);
          lastLightTurnedOn = millis();

          Serial.println("Renewing Address");
          uint16_t newAddress = mesh.renewAddress(5000);
          if (newAddress) {
            Serial.print("Address renewed as: "); Serial.println(newAddress);
          } else {
            Serial.println("Timeout while renewing address");
          }
        } else {
          renderStrip(&strip, 0, 100, 0);
          lastLightTurnedOn = millis();

          Serial.println("Send fail, Test OK");
        }
      } else {
        renderStrip(&strip, 100, 0, 0);
        lastLightTurnedOn = millis();

        Serial.print("Send OK: "); Serial.println(displayTimer);
      }
    }

    if (lastLightTurnedOn && (millis() - lastLightTurnedOn >= 400)) {
      clearStrip(&strip);
      lastLightTurnedOn = 0;
    }
  
    while (network.available()) {
      renderStrip(&strip, 100, 0, 100);
      RF24NetworkHeader header;
      payload_t payload;
      network.read(header, &payload, sizeof(payload));
      Serial.print("Received packet #");
      Serial.print(payload.counter);
      Serial.print(" at ");
      Serial.println(payload.ms);
      renderStrip(&strip, 0, 0, 0);
    }
  #endif
}
