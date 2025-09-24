#include <Adafruit_NeoPixel.h>
#include "Button.h"
#include "Knob.h"

const int LEDPIN = 6;
const int BUTTONPIN = 5;
const int KNOBPIN = 16;
const int frameLength = 33;

Button* button = new Button(BUTTONPIN);
Knob* knob = new Knob(KNOBPIN);

const int LEDS = 4;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_RGB + NEO_KHZ800);


float brightness = 0.8;
int mode = 0;
int numModes = 7;
unsigned int frame = 0;

int setPixel(int pixel, int R, int G, int B) {
  strip.setPixelColor(pixel, R * brightness, G * brightness, B * brightness);
}

float getBrightness(int knobVal) {
  float max = 1024.0;
  return float(knobVal) / max;
}

void render() {
  strip.show();
  delay(frameLength);
  strip.clear();
}

void setBase(int R, int G, int B) {
  for (int led = 0; led < LEDS; led++) {
    setPixel(led, R, G, B);
  }
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);

  strip.begin();
  strip.show();

  // Show red for a moment to signify setup is complete
  while (frame < 120) {
    RGB();
    render();
    frame++;
  }
  frame = 0;

}

//TODO seperate each chase into it's own class and header file
void RGB() {
  if (frame < 30) {
    setPixel(0, 200, 0, 0);
    setPixel(1, 0, 200, 0);
    setPixel(2, 0, 0, 200);
    setPixel(3, 0, 0, 0);
  } else if (frame < 60) {
    setPixel(0, 0, 0, 0);
    setPixel(1, 200, 0, 0);
    setPixel(2, 0, 200, 0);
    setPixel(3, 0, 0, 200);
  } else if (frame < 90) {
    setPixel(0, 0, 0, 200);
    setPixel(1, 0, 0, 0);
    setPixel(2, 200, 0, 0);
    setPixel(3, 0, 200, 0);
  } else {
    setPixel(0, 0, 200, 0);
    setPixel(1, 0, 0, 200);
    setPixel(2, 0, 0, 0);
    setPixel(3, 200, 0, 0);
  }
  frame++;
  
  if (frame > 120) {
    frame = 0;
  }
}

void warmWhite() {
  int R = 255;
  int G = 100;
  int B = 30;
  
  for (int led = 0; led < LEDS; led++) {
    setPixel(led, R, G, B);
  }
}

// TODO fill array randomly based on number of LEDs
int levels[LEDS][4] = {
  {0,0,0,0},
  {1,0,0,0},
  {0,0,0,0},
  {0,0,0,0}
};

void slowFire() {
  setBase(70, 15, 0);
  
  if (frame >= 50) {
      int spark = random(LEDS);
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
    setPixel(render, levels[render][1] + 70, levels[render][1]/2 + 15, 0);
  }
  frame++;
}

void ice() {
  setBase(10, 30, 40);
  
  if (frame >= 50) {
      int spark = random(LEDS);
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
    setPixel(render, levels[render][1] + 10, levels[render][1]/3 + 30, levels[render][1] + 40);
  }
  frame++;
}

int getColor(int index, int color[3]) {
  int cycle = 120;

  if (index < cycle) {
    color[2] = 0;
    color[1] = cycle - index;
    color[0] = index;
  } else if (index < (cycle*2)) {
    color[2] = index - cycle;
    color[1] = 0;
    color[0] = cycle - (index - cycle);
  } else {
    color[2] = cycle - (index - (cycle*2));
    color[1] = index - (cycle*2);
    color[0] = 0;
  }
};

void skittles() {
  if (frame >= 360) {
    frame = 0;
  }
  for (int pixel = 0; pixel < LEDS; pixel++) {
    int color[3];
    getColor((pixel * 40 + frame) % 360, color);
    setPixel(pixel, color[0], color[1], color[2]);
  }
  frame++;
}

int colors[LEDS][3] = {
  {0,0,0},
  {0,0,0},
  {0,0,0},
  {0,0,0}
};

int assignColor(int index, int color[3]) {
  colors[index][0] = color[0];
  colors[index][1] = color[1];
  colors[index][2] = color[2];
};

void disco() {
  int jump = random(7);
  if (jump == 0) {
    int color[3];
    getColor(random(360), color);
    assignColor(random(LEDS), color);
  }
for (int pixel = 0; pixel < LEDS; pixel++) {
    setPixel(pixel, colors[pixel][0], colors[pixel][1], colors[pixel][2]);
  }
}

void loop() {

  button->update();
  if (button -> wasShortPressed()) {
    mode = (mode + 1) % numModes;
    Serial.println("Mode: " + String(mode));
  } else if (button -> wasLongPressed()) {
    mode = 0;
    Serial.println("Mode: " + String(mode));
  }

  if (knob->update()) {
    brightness = getBrightness(knob->getValue());
    Serial.println("Brightness: " + String(brightness));
  }

  if (mode == 0) {
    // sleep
  } else if (mode == 1) {
    warmWhite();
  } else if (mode == 2) {
    slowFire();
  } else if (mode == 3) {
    ice();
  } else if (mode == 4) {
    skittles();
  } else if (mode == 5) {
    disco();
  } else if (mode == 6) {
    RGB();
  }
  render();
}
