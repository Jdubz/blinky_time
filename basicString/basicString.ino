/* Sketch By Josh Wentworth
 *  ToDo:
 *  Mode manager for easy registration/creation using common interface for modes
 *  open scource on gihub
 *   - document sources
 *   - circuit diagram
 *   - component links
 */


#include <Adafruit_NeoPixel.h>
#include "Mode.h"
#include "Utils.h"
#include "AudioLevelsMode.h"
#include "StarsMode.h"
#include "RunnerMode.h"
#include "Button.h"
//#include <nRF24L01.h>
//#include <RF24.h>

// Basic Inputs
int LEDPIN = 2;

// WiFi Radio Inputs
//int CSNPIN = 7;
//int CEPIN = 8;
//int MOSIPIN = 11;
//int MISOPIN = 12;
//int SCKPIN = 13;

// Sketch Constants
const int LEDS = 50; // TODO use strip.numPixels() instead of setting a constant

// Sketch Variables
int mode = 0;

Button* button = new Button(3);

//const byte address[6] = "00001";

//RF24 radio(CSNPIN, CEPIN);
Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

void renderStrip() {
  strip.show();

  // Clear all LEDs to avoid unwanted preservation of colors
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
}

// TODO create interface for ModeManager to easily register/unregister
// and handle mode execution


class AlternatingMode: public Mode {
  public:
    void run() {
      color colorValue = getSingleColorValue();
      color flippedValue = getFlippedColorOf(colorValue);
    
      // All colors divided by two to reduce brightness and
      // preserve battery
      for (int ledIndex = 0; ledIndex < LEDS; ledIndex++) {
        if (ledIndex % 2 == 0) {
          strip.setPixelColor(ledIndex,
            float(colorValue.green / 2),
            float(colorValue.red / 2),
            float(colorValue.blue / 2));
        } else {
          strip.setPixelColor(ledIndex,
            float(flippedValue.green / 2),
            float(flippedValue.red / 2),
            float(flippedValue.blue / 2));
        }
      }
    }
};



Mode *registeredModes[] = {
  new RunnerMode(&strip, LEDS),
  new AlternatingMode(),
  new AudioLevelsMode(&strip, &LEDS),
  new StarsMode(&strip, &LEDS)
};

void setup() {
  Serial.begin(9600);

  // Setup components
  strip.begin();
  strip.show();

  // Show red for a moment to signify setup is complete
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 150, 0);
  }
  strip.show();
  delay(100);
}

void loop() {
  // Update components
  button->update();
  renderStrip();

  if (button->wasShortPressed()) {
    mode = (mode + 1) % sizeof(registeredModes);
  }

  registeredModes[mode]->run();
}
