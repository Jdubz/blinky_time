#include <Adafruit_NeoPixel.h>
#include "RF24Network.h"
#include "RF24.h"
#include "RF24Mesh.h"
#include <SPI.h>
//Include eeprom.h for AVR (Uno, Nano) etc. except ATTiny
//#include <EEPROM.h>

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
 * Knob Stuff
 */
// Knob Reading with throttling
int currentKnobValue = 0;


bool readKnob() {
  static int lastKnobValue = 0;
  static int knobBeforeMove = 0;

  currentKnobValue = analogRead(KNOBPIN);
  bool result = false;
  bool isStopped = currentKnobValue == lastKnobValue;
  bool isDifferentFromStart = currentKnobValue != knobBeforeMove;

  // TODO: sometimes value jumps between two numbers and triggers a change
  if (isStopped && isDifferentFromStart) {
    result = true;
    knobBeforeMove = currentKnobValue;
  }

  lastKnobValue = currentKnobValue;
  return result;
}

/*
 * LED Stuff
 */
const int NUM_LEDS = 50;
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

/*
 * Network Stuff
 */
RF24 radio(CEPIN,CSNPIN);
RF24Network network(radio);
RF24Mesh mesh(radio,network);

struct payload_t {
  unsigned long ms;
  unsigned long data; // color or timestamp
};

void setup() {
  Serial.begin(9600);

  setupStrip(&strip);

  mesh.setNodeID(NODE_ID);
  Serial.print("Node Id set to "); Serial.println(mesh.getNodeID());
  // Connect to the mesh
  Serial.println("Connecting to the mesh...");
  mesh.begin();
  Serial.println("Mesh begun");

  clearStrip(&strip);
}


void loop() {
  #if defined(IS_MASTER)
    /* 
     *  Master Node
     */
    static uint32_t displayTimer = 0;

    // Call mesh.update to keep the network updated
    mesh.update();
  
    // In addition, keep the 'DHCP service' running on the master node so addresses will
    // be assigned to the sensor nodes
    mesh.DHCP();
  
    // Check for incoming data from the sensors
    if(network.available()){
      RF24NetworkHeader header;
      network.peek(header);
      Serial.print("Got ");
      uint32_t dat=0;
      switch(header.type){
        // Display the incoming millis() values from the sensor nodes
      case 'M': 
        network.read(header,&dat,sizeof(dat));
        Serial.print(dat);
        Serial.print(" from RF24Network address 0");
        Serial.println(header.from_node, OCT);
        break;
      default: 
        network.read(header,0,0); 
        Serial.print("Recieved message with unknown header type: "); Serial.println(header.type);
        break;
      }
    }
  
    // Send nodes new knob value
    if(readKnob()){
      // convert knob value (0-1023) to color (0-255) and type for payload
      unsigned long color = currentKnobValue / 4;
      renderStrip(&strip, color, color, color);
      Serial.println(F("********Sending to Nodes**********"));
      for (int i = 0; i < mesh.addrListTop; i++) {
        Serial.print("NodeID: ");
        Serial.print(mesh.addrList[i].nodeID);
        Serial.print(" RF24Network Address: 0");
        Serial.print(mesh.addrList[i].address,OCT);
        Serial.print(": ");
        
        payload_t payload = {millis(), color};
        
        RF24NetworkHeader header(mesh.addrList[i].address, 'C'); //Constructing a header
        if (network.write(header, &payload, sizeof(payload))) {
          Serial.println("Send OK");
        } else {
          Serial.println("Send Fail");
        }
      }
      Serial.println(F("**********************************"));
      displayTimer = millis();
    }
  #else
    /* 
     *  Child Node
     */
    static uint32_t displayTimer = 0;

    mesh.update();
  
    // Send to the master node every second
    if (millis() - displayTimer >= 1000) {
      displayTimer = millis();
  
      // Send an 'M' type message containing the current millis()
      if (!mesh.write(&displayTimer, 'M', sizeof(displayTimer))) {
  
        // If a write fails, check connectivity to the mesh network
        if ( ! mesh.checkConnection() ) {
          //refresh the network address
          Serial.println("Renewing Address");
          uint16_t newAddress = mesh.renewAddress(5000);
          if (newAddress) {
            Serial.print("Address renewed as: "); Serial.println(newAddress);
          } else {
            Serial.println("Timeout while renewing address");
          }
        } else {
          Serial.println("Send fail, Test OK");
        }
      } else {
        Serial.print("Send OK: "); Serial.println(displayTimer);
      }
    }
  
    if (network.available()) {
      RF24NetworkHeader header;
      payload_t payload;
      uint32_t color=0;
      network.read(header, &payload, sizeof(payload));
      switch(header.type){
      case 'C':
        // cap payload.data to be safe
        color = payload.data > 255 ? 255 : payload.data;
        // set strip color
        renderStrip(&strip, color, color, color);

        // Print to logs
        Serial.print(payload.data);
        Serial.print(" from RF24Network address 0");
        Serial.println(header.from_node, OCT);
        
        break;
      default: 
        network.read(header,0,0); 
        Serial.print("Node recieved message with unknown header type: "); Serial.println(header.type);
        break;
      }
    }
  #endif
}
