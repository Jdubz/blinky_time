#include <Adafruit_NeoPixel.h>

int LEDPIN = 2;
const int LEDS = 24;
int frameRate = 30;
int frame = 0;

int pixels[6][4] = {
  {0,1,2,3},
  {4,5,6,7},
  {8,9,10,11},
  {12,13,14,15},
  {16,17,18,19},
  {20,21,22,23}
};

int levels[6][2] = {
  {0,0},
  {1,0},
  {0,0},
  {0,0},
  {1,0},
  {0,0}
};

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

void renderStrip() {
  strip.show();
  delay(frameRate);
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 100, 0, 0);
  }
  frame++;
}

void setup() {
  Serial.begin(9600);
  
  strip.begin();
  strip.show();

  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 150, 0);
  }
  strip.show();
  delay(1000);
}

void loop() {
  if (frame == 50) {
    int spark = random(6);
    Serial.println(spark);
    levels[spark][0] = 1;
    frame = 0;
  }
  
  for (int pixel = 0; pixel < 6; pixel++) {
    if (levels[pixel][0] == 1 && levels[pixel][1] < 100) {
      levels[pixel][1]++;
    } else if (levels[pixel][0] == 1 && levels[pixel][1] == 100) {
      levels[pixel][0] = 0;
    } else if (levels[pixel][0] == 0 && levels[pixel][1] > 0) {
      levels[pixel][1]--;
    }
  }

  for (int render = 0; render < 6; render++) {
    for (int four = 0; four < 4; four++) {
      strip.setPixelColor(pixels[render][four], levels[render][1] + 100, levels[render][1], 0);
    }
  }
  
  renderStrip();
}
