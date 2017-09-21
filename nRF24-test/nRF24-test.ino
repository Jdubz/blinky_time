#include <nRF24L01.h>
#include <RF24.h>
#include <SPI.h>
#include <Adafruit_NeoPixel.h>

int CSNPIN = 7;
int CEPIN = 8;
int MOSIPIN = 11;
int MISOPIN = 12;
int SCKPIN = 13;

int LEDPIN = 2;
int LEDS = 50;

RF24 radio(CSNPIN, CEPIN);
Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

const byte address[6] = "00125";

void setup() {
  Serial.begin(9600);
  radio.begin();
  radio.openWritingPipe(address);
  radio.setPALevel(RF24_PA_MIN);
  radio.stopListening();
  strip.begin();
  strip.show();
}

void loop() {

  int msg[1];
  bool wrote;
  msg[0] = 27;
  wrote = radio.write(msg, 1);
  if (wrote) {
    for (int led = 0; led < LEDS; led++) {
      strip.setPixelColor(led, 0, 50, 0);
    }
    strip.show();
  } else {
    for (int led = 0; led < LEDS; led++) {
      strip.setPixelColor(led, 0, 0, 0);
    }
    strip.show();
  }
  Serial.println(wrote);
  delay(1000);
}
