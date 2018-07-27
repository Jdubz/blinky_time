#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>

#include <Adafruit_NeoPixel.h>

const int LEDPIN = 2;
const int LEDS = 17;
const int frameRate = 30;
int frame = 0;

const int CEPIN = 9;
const int CSNPIN = 10;

RF24 radio(CEPIN, CSNPIN); // CE, CSN
const byte address[6] = "00001";

void startRadio() {
  radio.begin();
  radio.openReadingPipe(0, address);
  radio.setPALevel(RF24_PA_MIN);
  radio.startListening();
}

void checkRadio() {
  if (radio.available()) {
    int readRadio[2];
    radio.read(readRadio, sizeof(readRadio));
    Serial.println(readRadio[1]);
  }
}

int ledLvls[LEDS][2];

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

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
  
  for (int star = 0; star < LEDS; star++) {
    if (ledLvls[star][0] > 0) {
      if (ledLvls[star][1] == 1) {
        if (ledLvls[star][0] < 100) {
          ledLvls[star][0] +=5;
        } else {
          ledLvls[star][1] = 0;
        }
      } else {
        ledLvls[star][0] -=5;
      }
      float starLvl = ledLvls[star][0] / 100.0;
      strip.setPixelColor(star, 150 * starLvl, 0, 0);
    }
  }
  if (frame == 5) {
    int newStar = random(LEDS);
    int newStar2 = random(LEDS);
    int newStar3 = random(LEDS);
    int newStar4 = random(LEDS);
    ledLvls[newStar][0] = 1;
    ledLvls[newStar][1] = 1;
    ledLvls[newStar2][0] = 1;
    ledLvls[newStar2][1] = 1;
    ledLvls[newStar3][0] = 1;
    ledLvls[newStar3][1] = 1;
    ledLvls[newStar4][0] = 1;
    ledLvls[newStar4][1] = 1;
    frame = 0;
  }
  frame++;
  renderStrip();
}
