#include <Adafruit_NeoPixel.h>
#include <SPI.h>
#include "RH_NRF24.h"


/*
 * Pin Constants
 */
const int LEDPIN = 2;
const int BUTTONPIN = 3;
const int CEPIN = 9;
const int CSNPIN = 10;

/*
 * LEDs
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

/*
 * States
 */

enum state_e {
  lfg_bro,
  relaying_network_status,
};

state_e my_state;

/*
 * Message
 */

struct payload_t {
  // variables
  int mutation_count;
  int sender_id;
  state sender_state;
}

/*
 * Radio
 */

RH_NRF24 nrf24;

/*
 * Application
 */

void setup() {
  Serial.begin(9600);

  setupState();
  setupLEDs();
  setupRadio();
}

void setupState() {
  my_state = lfg_bro;
}

void setupLEDs() {
  setupStrip(&strip);
  clearStrip(&strip);
}

void setupRadio() {
  while (!Serial)
    ; // wait for serial port to connect. Needed for Leonardo only
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


void loop() {
  switch(my_state) {
    case lfg_bro:
      doLfgGroup();
      break;
    case relaying_network_status:
      doRelayingNetworkStatus();
      break;
    default:
      Serial.println("I'm in an unknown state"); 
      break;
  }
}

// Sample message read code
//  if (nrf24.available()) {
//    // Should be a message for us now   
//    uint8_t buf[RH_NRF24_MAX_MESSAGE_LEN];
//    uint8_t len = sizeof(buf);
//    
//    if (nrf24.recv(buf, &len)) {
//      Serial.print("got request: ");
//      Serial.println((char*)buf);
//    } else {
//      Serial.println("recv failed");
//    }
//  }

// Sample message send code
//      uint8_t data[] = "And hello back to you";
//      nrf24.send(data, sizeof(data));
//      nrf24.waitPacketSent();
//      Serial.println("Sent a reply");

void doLfgBro() {
  // read messages
  // if got lfg, create a group, switch state
  // if got network, join network, mutate and switch state
  // else, send lfg
}

void doRelayingNetworkStatus() {
  // read messages
  // if got network, check mutations count and id of message producer, to see if should change
  // if got lfg, send network status
  // if button hit, change vars, send network status
}

