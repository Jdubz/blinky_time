#include <Adafruit_NeoPixel.h>

const int LEDPIN = 2;
const int BUTTONPIN = 3;
const int NUMLEDS = 5;
const int DELAYTIME = 30;
const float brightness = 150.0;

int pressed = 0;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMLEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

typedef struct colorValues {
  uint8_t green;
  uint8_t red;
  uint8_t blue;
}
color;

int colorVal;
float freq;
float phase;

void randomize() {
  colorVal = random(99);
  freq = random(100) / 100.0;
  phase = random(100) / 100.0;
  Serial.println(String(colorVal) + " " + String(freq) + " " + String(phase));
}

color getSingleColorValue() {
  color colorValue;
  float ramp = (colorVal % 33) / 33.0;
  if (colorVal <= 33) {
    colorValue.green = int(ramp * brightness);
    colorValue.red = int((1.0 - ramp) * brightness);
    colorValue.blue = 0;
  } else if (colorVal <= 66) {
    colorValue.blue = int(ramp * brightness);
    colorValue.green = int((1.0 - ramp) * brightness);
    colorValue.red = 0;
  } else if (colorVal > 66) {
    colorValue.red = int(ramp * brightness);
    colorValue.blue = int((1.0 - ramp) * brightness);
    colorValue.green = 0;
  }

  return colorValue;
}

void pendulumStep() {
  const float base = 0.03;
  colorValues colors = getSingleColorValue();
  for (int led = 0; led < NUMLEDS; led++) {
    int waveLength = int(100.0 * (base + (led / 100.0)));
    int offset = int(phase * 100.0) % waveLength;
    float height;
    if (offset < (waveLength / 2)) {
      height = offset / (waveLength / 2);
    } else {
      height = 1.0 - (offset % (waveLength / 2));
    }
    strip.setPixelColor(led, colors.green * height, colors.red * height, colors.blue * height);
  }
  phase = phase + freq;
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
  randomize();
}

void loop() {

  if (digitalRead(BUTTONPIN) == HIGH && pressed == 0) {
    pressed = 1;
    randomize();
  } else if (pressed == 1 && digitalRead(BUTTONPIN) == LOW) {
    pressed = 0;
  }

  pendulumStep();
  
  render();
};
