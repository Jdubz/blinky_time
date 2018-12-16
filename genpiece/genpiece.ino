#include <Adafruit_NeoPixel.h>

const int LEDPIN = 2;
const int LEDS = 9;
const int BUTTONPIN = 3;
const int NUMMODES = 4;

int frameRate = 30;
int frame = 0;


int mode = 3;
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
  
  if (frame >= 50) {
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
  
  if (frame >= 50) {
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

int getColor(int index, int color[3]) {
  int brightness = 120;
 
  if (index < 120) {
    color[2] = 0;
    color[1] = brightness - index;
    color[0] = index;
  } else if (index < 240) {
    color[2] = index - 120;
    color[1] = 0;
    color[0] = brightness - (index - 120);
  } else {
    color[2] = brightness - (index - 240);
    color[1] = index - 240;
    color[0] = 0;
  }
}

void skittles() {
  int base[3] = {0,0,0};

  if (frame >= 360) {
    frame = 0;
  }

  int printCol[3];
  getColor(frame, printCol);
  Serial.println(String(printCol[0]) + ' ' + String(printCol[1]) + ' ' + String(printCol[2]));
  for (int pixel = 0; pixel < LEDS; pixel++) {
    int color[3];
    getColor((pixel * 40 + frame) % 360, color);
    strip.setPixelColor(pixel, color[0], color[1], color[2]);
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
  } else if (mode == 3) {
    skittles();
  }
}
