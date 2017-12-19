
/** RF24Mesh_Example.ino by TMRh20

   This example sketch shows how to manually configure a node via RF24Mesh, and send data to the
   master node.
   The nodes will refresh their network address as soon as a single write fails. This allows the
   nodes to change position in relation to each other and the master node.
*/

#include <Adafruit_NeoPixel.h>
#include "RF24.h"
#include "RF24Network.h"
#include "RF24Mesh.h"
#include <SPI.h>
//#include <printf.h>

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
  strip->show();
  renderStrip(strip, 0, 100, 0);
  strip->show();
}

/**** Configure the nrf24l01 CE and CS pins ****/
RF24 radio(CEPIN, CSNPIN);
RF24Network network(radio);
RF24Mesh mesh(radio, network);

/**
   User Configuration: nodeID - A unique identifier for each radio. Allows addressing
   to change dynamically with physical changes to the mesh.

   In this example, configuration takes place below, prior to uploading the sketch to the device
   A unique value from 1-255 must be configured for each node.
   This will be stored in EEPROM on AVR devices, so remains persistent between further uploads, loss of power, etc.

 **/
#define nodeID 1


uint32_t displayTimer = 0;

struct payload_t {
  unsigned long ms;
  unsigned long counter;
};

void setup() {

  Serial.begin(9600);
  //printf_begin();

  setupStrip(&strip);
  
  // Set the nodeID manually
  mesh.setNodeID(nodeID);
  // Connect to the mesh
  Serial.println(F("Connecting to the mesh..."));
  mesh.begin();
  Serial.println("Mesh begun");

  renderStrip(&strip, 0, 0, 0);
}



void loop() {
//  Serial.println("pre mesh update");
  mesh.update();
//  Serial.println("post mesh update");

  // Send to the master node every second
  if (millis() - displayTimer >= 1000) {
    renderStrip(&strip, 0, 0, 100);
    displayTimer = millis();

    // Send an 'M' type message containing the current millis()
    if (!mesh.write(&displayTimer, 'M', sizeof(displayTimer))) {

      // If a write fails, check connectivity to the mesh network
      if ( ! mesh.checkConnection() ) {
        //refresh the network address
        renderStrip(&strip, 0, 0, 100);
        Serial.println("Renewing Address");
        uint16_t newAddress = mesh.renewAddress(5000);
        if (newAddress) {
          Serial.print("Address renewed as: "); Serial.println(newAddress);
        } else {
          Serial.println("Timeout while renewing address");
        }
      } else {
        renderStrip(&strip, 0, 100, 0);
        Serial.println("Send fail, Test OK");
      }
    } else {
      renderStrip(&strip, 100, 0, 0);
      Serial.print("Send OK: "); Serial.println(displayTimer);
    }

    clearStrip(&strip);
  }

  while (network.available()) {
    renderStrip(&strip, 100, 0, 0);
    RF24NetworkHeader header;
    payload_t payload;
    network.read(header, &payload, sizeof(payload));
    Serial.print("Received packet #");
    Serial.print(payload.counter);
    Serial.print(" at ");
    Serial.println(payload.ms);
    clearStrip(&strip);
  }
}
