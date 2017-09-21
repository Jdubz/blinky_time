#include <Adafruit_NeoPixel.h>
int LEDPIN = 2;
const int LEDS = 50;
int frameRate = 30;
int frame = 0;

int ledLvls[LEDS][2];

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

void renderStrip() {
  strip.show();
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
  delay(frameRate);
}

void setup() {
  strip.begin();
  strip.show();

  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 150, 0);
  }
  strip.show();
  delay(100);
}

void loop() {
  for (int star = 0; star < LEDS; star++) {
    if (ledLvls[star][0] > 0) {
      if (ledLvls[star][1] == 1) {
        if (ledLvls[star][0] < 100) {
          ledLvls[star][0]++;
        } else {
          ledLvls[star][1] = 0;
        }
      } else {
        ledLvls[star][0]--;
      }
      float starLvl = ledLvls[star][0] / 100.0;
      strip.setPixelColor(star, 150 * starLvl, 0, 0);
    }
  }
  if (frame == 5) {
    int newStar = random(50);
    int newStar2 = random(50);
    ledLvls[newStar][0] = 1;
    ledLvls[newStar][1] = 1;
    ledLvls[newStar2][0] = 1;
    ledLvls[newStar2][1] = 1;
    frame = 0;
  }
  frame++;
  renderStrip();
}
