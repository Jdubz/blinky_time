#include <Adafruit_NeoPixel.h>

const int LEDPIN = 2;
const int MICPIN = A0; // 0 - 600
const int BUTTONPIN = 3;
const int NUMLEDS = 106;
const int DELAYTIME = 30;
const int NUMMODES = 2;

int mode = 1;
int pressed = 0;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMLEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

//39, 36, 31

int threshhold = 100;
float gain = 50.0;

float getMicLevel() {
  int lvl1;
  unsigned long start = millis();
  int high = 0;
  int low = 1024;
  while (millis() - start < DELAYTIME) {
    int sample = analogRead(MICPIN);
    if (sample < low) {
      low = sample;
    } else if (sample > high) {
      high = sample;
    }
  }
  lvl1 = high - low;
  float micLevel = lvl1 / 1024.0;
  if (threshhold > 20) {
    threshhold = threshhold - 1;
  }
  if (lvl1 > threshhold) {
    threshhold = lvl1;
  }
  gain = 1024.0 / threshhold;
  // Serial.println(String(micLevel) + " : " + String(gain));
  return micLevel * gain;
}

void render() {
  strip.show();
  for (int led = 0; led < NUMLEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
}

int sparks[NUMLEDS][3];
void setup() {
  pinMode(BUTTONPIN, INPUT);
  Serial.begin(9600);
  strip.begin();
  strip.show();
}

void loop() {
  float micLvl = getMicLevel();

  if (digitalRead(BUTTONPIN) == HIGH && pressed == 0) {
    mode = (mode + 1) % NUMMODES;
    pressed = 1;
  } else if (pressed == 1 && digitalRead(BUTTONPIN) == LOW) {
    pressed = 0;
  }

  if (mode == 0) {
    // sleep
  } else if (mode == 1) {
    int newSparks = 0;
    if (micLvl > 0.5) {
      newSparks = micLvl * 10;
    } else if (random(7) == 6) {
      newSparks = 1;
    }
    for (int newSpark = 0; newSpark < newSparks; newSpark++) {
      int sparkId = random(NUMLEDS);
      sparks[sparkId][0] = 80;
      sparks[sparkId][1] = random(4) * (micLvl * 15);
      sparks[sparkId][2] = 1;
    }
    for (int thisSpark = 0; thisSpark < NUMLEDS; thisSpark++) {
      if (sparks[thisSpark][2] == 1) {       
        if (sparks[thisSpark][1] > 2) {
          sparks[thisSpark][1] -= 2;
        } else if (sparks[thisSpark][1] != 0) {
          Serial.println(sparks[thisSpark][1]);
        }
        if (sparks[thisSpark][0] > 2) {
          sparks[thisSpark][0] -= 2;
        } else {
          sparks[thisSpark][0] = 0;
          sparks[thisSpark][1] = 0;
          sparks[thisSpark][2] = 0;
        } 
      }
      strip.setPixelColor(thisSpark, sparks[thisSpark][0], sparks[thisSpark][1], 0);
    }
  }

  render();
};

