#include <Adafruit_NeoPixel.h>

const int LEDPIN = 2;
const int LEDS = 9;
const int BUTTONPIN = 3;
const int NUMMODES = 3;

int frameRate = 30;
int frame = 0;


int mode = 2;
int pressed = 0;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

int renderStrip(int G, int R, int B) {
  strip.show();
  delay(frameRate);
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, G, R, B);
  }
  frame++;
}

void sleep() {
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
  strip.show();
}

void setup() {
  Serial.begin(9600);
  
  strip.begin();
  strip.show();

  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 20, 50);
  }
  strip.show();
  delay(1000);
}

void buttonCheck() {
  if (digitalRead(BUTTONPIN) == HIGH && pressed == 0) {
    mode = (mode + 1) % NUMMODES;
    pressed = 1;
    if (mode == 0) {
      renderStrip(0, 0, 0);
    }
  } else if (pressed == 1 && digitalRead(BUTTONPIN) == LOW) {
    pressed = 0;
  }
}

int levels[9][4] = {
  {0,0,0,0},
  {1,0,0,0},
  {0,0,0,0},
  {0,0,0,0},
  {1,0,0,0},
  {0,0,0,0},
  {1,0,0,0},
  {0,0,0,0},
  {0,0,0,0}
};

void slowFire() {
  int base[3] = {20, 50, 0};
  
  if (frame == 50) {
      int spark = random(LEDS);
      Serial.println(spark);
      levels[spark][0] = 1;
      frame = 0;
    }
  
    for (int pixel = 0; pixel < LEDS; pixel++) {
      if (levels[pixel][0] == 1 && levels[pixel][1] < 150) {
        levels[pixel][1]++;
      } else if (levels[pixel][0] == 1 && levels[pixel][1] == 150) {
        levels[pixel][0] = 0;
      } else if (levels[pixel][0] == 0 && levels[pixel][1] > 0) {
        levels[pixel][1]--;
      }
    }

    for (int render = 0; render < LEDS; render++) {
      strip.setPixelColor(render, levels[render][1] + base[0], levels[render][1] + base[1], base[2]);
    }
    renderStrip(base[0], base[1], base[2]);
}

void ice() {
  int base[3] = {30, 10, 40};
  
  if (frame == 50) {
      int spark = random(LEDS);
      Serial.println(spark);
      levels[spark][0] = 1;
      frame = 0;
    }
  
    for (int pixel = 0; pixel < LEDS; pixel++) {
      if (levels[pixel][0] == 1 && levels[pixel][1] < 150) {
        levels[pixel][1]++;
      } else if (levels[pixel][0] == 1 && levels[pixel][1] == 150) {
        levels[pixel][0] = 0;
      } else if (levels[pixel][0] == 0 && levels[pixel][1] > 0) {
        levels[pixel][1]--;
      }
    }

    for (int render = 0; render < LEDS; render++) {
      strip.setPixelColor(render, levels[render][1]/2 + base[0], levels[render][1]/3 + base[1], levels[render][1] + base[2]);
    }
    renderStrip(base[0], base[1], base[2]);
}

void loop() {
  buttonCheck();
  
  if (mode == 0) {
    sleep();
  } else if (mode == 1) {
    slowFire();
  } else if (mode == 2) {
    ice();
  }
}
