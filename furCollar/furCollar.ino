#include <Adafruit_NeoPixel.h>

const int LEDPIN = 2;
const int MICPIN = A0; // 0 - 600
const int BUTTONPIN = 3;
const int NUMLEDS = 106;
const int DELAYTIME = 30;
const int NUMMODES = 2;

int mode = 1;
int pressed = 0;

int xPos = 0;
int yPos = 0;
int lvl2 = 0;
int frame = 0;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMLEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

//39, 36, 31

int stars[NUMLEDS];
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

void setup() {
  pinMode(BUTTONPIN, INPUT);
  Serial.begin(9600);
  strip.begin();
  strip.show();
}

void loop() {
  render();
  float micLvl = getMicLevel();

  if (digitalRead(BUTTONPIN) == HIGH && pressed == 0) {
    mode = (mode + 1) % NUMMODES;
    pressed = 1;
    frame = 0;
    // Serial.println(mode);
  } else if (pressed == 1 && digitalRead(BUTTONPIN) == LOW) {
    pressed = 0;
  }

  if (mode == 0) {
    // do nothing
  } else if (mode == 1) {
    int create = random(3);
    int newStars;
    if (create == 0) {
      newStars = random(1,3);
    } else {
      newStars = 0;
    }
    int createStars = newStars + ceil(micLvl * 20);
    for (int num = 0; num < createStars; num++) {
      stars[random(NUMLEDS)] = 150;
    }
    for (int num2 = 0; num2 < NUMLEDS; num2++) {
      strip.setPixelColor(num2, 0, 0, stars[num2]);
      if (stars[num2] > 20) {
        stars[num2] = stars[num2] - 20;
      } else {
        stars[num2] = 0;
      }
    }
  }
};

