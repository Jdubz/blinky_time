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

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

float brightness;
int mode = 0;
int numModes = 3;
unsigned int frame = 0;

float getBrightness(int knobVal) {
  float max = 1024.0;
  return float(knobVal) / max;
}

void render () {
  strip.show();
  Serial.println("Frame: " + String(frame));
  delay(frameLength);
  strip.clear();
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);

  strip.begin();
  strip.show();

  // Show red for a moment to signify setup is complete
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 150, 0, 0);
  }
  strip.show();
  delay(1000);

}

// mode specific variables
// mode 2


void loop() {

  button->update();
  if (button -> wasShortPressed()) {
    mode = (mode + 1) % numModes;
    Serial.println("Mode: " + String(mode));
  }

  if (knob->update()) {
    brightness = getBrightness(knob->getValue());
    Serial.println("Brightness: " + String(brightness));
  }

  if (mode == 0) {
    // sleep
  } else if (mode == 1) {
    
    if (frame < 30) {
      strip.setPixelColor(0, 200, 0, 0);
    } else if (frame < 60) {
      strip.setPixelColor(1, 200, 0, 0);
    } else if (frame < 90) {
      strip.setPixelColor(2, 200, 0, 0);
    } else if (frame < 120) {
      strip.setPixelColor(3, 200, 0, 0);
    }
    frame++;
    
    if (frame >= 120) {
      frame = 0;
    }
    
  } else if (mode == 2) {
    
    int R = 255 * brightness;
    int G = 100 * brightness;
    int B = 30 * brightness;
    
    for (int led = 0; led < LEDS; led++) {
      strip.setPixelColor(led, R, G, B);
    }
  }

  render();

}
