/* Sketch By Josh Wentworth
 *  ToDo:
 *  Mode manager for easy registration/creation using common interface for modes
 *  open scource on gihub
 *   - document sources
 *   - circuit diagram
 *   - component links
 */


#include <Adafruit_NeoPixel.h>
//#include <nRF24L01.h>
//#include <RF24.h>

// Basic Inputs
int LEDPIN = 2;
int KNOBPIN = 14;
int MICPIN = 15;

// WiFi Radio Inputs
//int CSNPIN = 7;
//int CEPIN = 8;
//int MOSIPIN = 11;
//int MISOPIN = 12;
//int SCKPIN = 13;

// Sketch Constants
const int LEDS = 50; // TODO use strip.numPixels() instead of setting a constant
int sampleSize = 30;
int numModes = 4;

// Sketch Variables
int frame = 0;
int mode = 0;
int knobIn = 0;

// TODO use library friendly data with strip.Color instead of this
// Colors stored and set in the order of
// green, red, blue not RGB
typedef struct colorValues {
  uint8_t green;
  uint8_t red;
  uint8_t blue;
}
color;

// TODO move this to its own file and import
// TODO refactor to make presses feel more immediate
class Button {
  public:
    Button(int inputPin) {
      pin = inputPin;
      pinMode(inputPin, INPUT);
      wasDown = false;
      wasPressed = false;
      longPressDuration = 1000;
      pressDuration = -1;
    }
    void update() {
      bool isDown = digitalRead(this->pin) == HIGH;
      bool isUp = digitalRead(this->pin) == LOW;

      if (isDown && !this->wasDown) {
        this->wasDown = true;
        this->pressStart = millis();
      } else if (isUp && this->wasDown) {
        this->pressDuration = millis() - this->pressStart;
        this->wasDown = false;
        this->wasPressed = true;
      } else {
        this->wasPressed = false;
      }
    }
    bool wasShortPressed() {
      return this->wasPressed && this->pressDuration < this->longPressDuration;
    }
    bool wasLongPressed() {
      return this->wasPressed && this->pressDuration >= this->longPressDuration;
    }
  private:
    int pin;
    bool wasDown;
    bool wasPressed;
    float pressStart;
    float longPressDuration;
    float pressDuration;
};

int threshhold = 100;
float gain = 50.0;

Button button = Button(3);

int ledLvls[LEDS][2];
//const byte address[6] = "00001";

//RF24 radio(CSNPIN, CEPIN);
Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

void keepBatteryOn() {
  // Keep every fifth light on so phone battery doesn't turn off
  color colorValue = getSingleColorValue();

  for (int on = 0; on < 5; on++) {
    strip.setPixelColor(on * 10,
      colorValue.green,
      colorValue.red,
      colorValue.blue);
  }
}

float getMicLevel() {
  int lvl1;
  unsigned long start = millis();
  int high = 0;
  int low = 1024;
  while (millis() - start < sampleSize) {
    int sample = analogRead(MICPIN);
    if (sample < low) {
      low = sample;
    } else if (sample > high) {
      high = sample;
    }
  }
  lvl1 = high - low;
  float micLvl = lvl1 / 1024.0;
  if (threshhold > 20) {
    threshhold = threshhold - 1;
  }
  if (lvl1 > threshhold) {
    threshhold = lvl1;
  }
  gain = 1024.0 / threshhold;
  micLvl = micLvl * gain;

  return micLvl;
}

color getSingleColorValue() {
  color colorValue;
  knobIn = analogRead(KNOBPIN);
  // which third of circle is the knob in
  int phase = knobIn / 341.333;
  float ramp = (knobIn % 342) / 341.333;
  if (phase == 0) {
    colorValue.green = int(ramp * 255.0);
    colorValue.red = int((1.0 - ramp) * 255.0);
    colorValue.blue = 0;
  } else if (phase == 1) {
    colorValue.blue = int(ramp * 255.0);
    colorValue.green = int((1.0 - ramp) * 255.0);
    colorValue.red = 0;
  } else if (phase == 2) {
    colorValue.red = int(ramp * 255.0);
    colorValue.blue = int((1.0 - ramp) * 255.0);
    colorValue.green = 0;
  }

  return colorValue;
}

color getFlippedColorOf(color referenceColor) {
  color flipped;
  flipped.green = 255 - referenceColor.green;
  flipped.red = 255 - referenceColor.red;
  flipped.blue = 255 - referenceColor.blue;

  return flipped;
}

void renderStrip() {
  strip.show();

  // Clear all LEDs to avoid unwanted preservation of colors
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
}

class Mode {
  public:
    virtual void run() = 0;
};

// TODO create interface for ModeManager to easily register/unregister
// and handle mode execution
class StarsMode: public Mode {
  public:
    StarsMode() {
    }
    void run() {
      float micLevel = getMicLevel();
      color colorValue = getSingleColorValue();

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
          strip.setPixelColor(star,
            float(colorValue.green) * micLevel * starLvl,
            float(colorValue.red) * micLevel * starLvl,
            float(colorValue.blue) * micLevel * starLvl);
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
    }
  private:
    int ledLvls[50][2];
};

class AudioLevelsMode: public Mode {
  public:
    void run() {
      float micLevel = getMicLevel();
      color colorValue = getSingleColorValue();
    
      for (int test = 0; test < LEDS; test++) {
        strip.setPixelColor(test,
          float(colorValue.green) * micLevel,
          float(colorValue.red) * micLevel,
          float(colorValue.blue) * micLevel);
      }
    
      keepBatteryOn();
    }
};

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

class Runner {
  public:
    Runner() {
      headIndex = 0;
      tailLength = 5;
      runThrottle = 15;
      runCallCount = 0;
      totalPixels = strip.numPixels();
    }
    void run() {
      this->handleThrottling();

      int lastIndex = this->headIndex;

      color currentColor = getSingleColorValue();

      for (int count = 0; count < this->tailLength; count++) {
        float diminish = float(this->tailLength - count)/float(this->tailLength);
        strip.setPixelColor(this->headIndex - count,
          currentColor.green * diminish,
          currentColor.red * diminish,
          currentColor.blue * diminish);
      }
    }
    void setHeadIndex(int newHeadIndex) {
      this->headIndex = newHeadIndex;
    }
    int getHeadIndex() {
      return this->headIndex;
    }
  private:
    void handleThrottling() {
      this->runCallCount++;

      if (this->runCallCount >= this->runThrottle) {
        this->headIndex++;

        // reset to 0 if out of bounds
        if (this->headIndex > this->totalPixels) {
          this->headIndex = 0;
        }

        this->runCallCount = 0;
      }
    }
    int headIndex;
    int tailLength;
    // determines throttle, lower is faster, should probably change this
    int runThrottle;
    int runCallCount;
    int totalPixels;
};

class RunnerMode: public Mode {
  public:
    RunnerMode() {
      runner1 = Runner();
      runner1.setHeadIndex(0);
      runner2 = Runner();
      runner2.setHeadIndex(LEDS / 4);
      runner3 = Runner();
      runner3.setHeadIndex(LEDS / 4 * 2);
      runner4 = Runner();
      runner4.setHeadIndex(LEDS / 4 * 3);
    }
    void run() {
      runner1.run();
      runner2.run();
      runner3.run();
      runner4.run();
    }
  private:
    Runner runner1;
    Runner runner2;
    Runner runner3;
    Runner runner4;
};

Mode *registeredModes[] = {
  new RunnerMode(),
  new StarsMode(),
  new AlternatingMode(),
  new AudioLevelsMode()
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
  button.update();
  renderStrip();

  if (button.wasShortPressed()) {
    mode = (mode + 1) % sizeof(registeredModes);
  }

  registeredModes[mode]->run();
}
