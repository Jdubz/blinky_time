#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>

#include <Adafruit_NeoPixel.h>

const int LEDPIN = 2;
const int LEDS = 17;
const int frameRate = 30;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

const int CEPIN = 9;
const int CSNPIN = 10;
int RecievedMessage[1] = {0};

RF24 radio(CEPIN, CSNPIN); // CE, CSN
const uint64_t pipe = 0xE6E6E6E6E6E6;

void startRadio() {
  radio.begin();
  radio.openReadingPipe(1, pipe);
//   radio.setPALevel(RF24_PA_MIN);
  radio.startListening();
}

void checkRadio() {
  while (radio.available()) {
    radio.read(RecievedMessage, 1);
    Serial.print(RecievedMessage[0]);
  }
}

void renderStrip() {
  strip.show();
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
  delay(1000/frameRate);
}

void setup() {
  strip.begin();
  strip.show();

  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 50, 0);
  }
  strip.show();
  delay(1000);

  startRadio();
}

void loop() {
  checkRadio();
//
//  for (int led = 0; led < LEDS; led++) {
//    strip.setPixelColor(led, RecievedMessage[1], 0, 0);
//  }
//  
//  renderStrip();
}
