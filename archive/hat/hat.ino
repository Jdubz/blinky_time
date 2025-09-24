#include <Adafruit_NeoPixel.h>

#define LED_PIN     2
#define NUM_LEDS    72
#define PULL_PIN    5

const int frameRate = 30;
bool isLow = false;

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

void pullKey() {
  int pingFreq = 10;
  int now = millis();
  int seconds = now/1000;
  if (!(seconds % pingFreq) && !isLow) {
    // Serial.print("Pull Low");
    // Serial.println();
    digitalWrite(PULL_PIN, LOW);
    isLow = true;
  } else if ((seconds % pingFreq) && isLow) {
    // Serial.print("Pull High");
    // Serial.println();
    digitalWrite(PULL_PIN, HIGH);
    isLow = false;
  }
}

void renderStrip() {
  strip.show();
  clear();
  delay(1000/frameRate);
}

void clear() {
  for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
}

void startup() {
  digitalWrite(PULL_PIN, LOW);
  for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, 0, 50, 0);
  }
  strip.show();
  delay(500);
    for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, 50, 0, 0);
  }
  strip.show();
  delay(500);
    for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 50);
  }
  strip.show();
  delay(500);
  digitalWrite(PULL_PIN, HIGH);
  clear();
  strip.show();
  delay(1000);
}

void setup() {
  pinMode(PULL_PIN, OUTPUT);
  // Serial.begin(9600);

  delay(3000);
  strip.begin();
  startup();
}

int sparks[NUM_LEDS][3];

void loop() {
  pullKey();

  if (random(4) == 1) {
    int sparkId = random(NUM_LEDS);
    sparks[sparkId][0] = 80;
    sparks[sparkId][1] = random(4) * 10;
    sparks[sparkId][2] = 1;
  }

  for (int thisSpark = 0; thisSpark < NUM_LEDS; thisSpark++) {
    if (sparks[thisSpark][2] == 1) {       
      if (sparks[thisSpark][1] > 2) {
        sparks[thisSpark][1] -= 2;
      }
      if (sparks[thisSpark][0] > 2) {
        sparks[thisSpark][0] -= 2;
      } else {
        sparks[thisSpark][0] = 0;
        sparks[thisSpark][1] = 0;
        sparks[thisSpark][2] = 0;
      } 
    }
    strip.setPixelColor(thisSpark, sparks[thisSpark][0], sparks[thisSpark][1], 50);
  }
  
  renderStrip();
}
