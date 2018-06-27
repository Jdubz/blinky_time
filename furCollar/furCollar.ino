#include <Adafruit_NeoPixel.h>

int LEDPIN = 2;
int MICPIN = A0; // 0 - 600
int BUTTONPIN = 3;
int NUMLEDS = 106;
int DELAYTIME = 30;
int NUMMODES = 5;

int mode = 0;
int pressed = 0;

int xPos = 0;
int yPos = 0;
int lvl2 = 0;
int frame = 0;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMLEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

//39, 36, 31

int threshhold = 100;
float gain = 6.0;
int lvl1;

void audioTune() {
  if (threshhold > 30) {
    threshhold = threshhold - 1;
  }
  gain = 300.0 / threshhold;
  lvl1 = analogRead(MICPIN);
  if (lvl1 > threshhold) {
    threshhold = lvl1;
  }
  lvl1 = lvl1 * gain; 
}

void render() {
  strip.show();
  delay(DELAYTIME);
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
  audioTune();

  if (digitalRead(BUTTONPIN) == HIGH && pressed == 0) {
    mode = (mode + 1) % NUMMODES;
    pressed = 1;
    frame = 0;
    Serial.println(mode);
  } else if (pressed == 1 && digitalRead(BUTTONPIN) == LOW) {
    pressed = 0;
  }
  
  if (mode == 0) {
    int rate = 30;
    xPos = floor(frame/rate);
    for (int yPos = 0; yPos < Y; yPos++) {
      if (frame % 30 > 20) {
        strip.setPixelColor(bucketMap[xPos][yPos], 0, 0, 100);
      } else if (frame % 30 > 10) {
        strip.setPixelColor(bucketMap[xPos][yPos], 100, 0, 0);
      } else {
        strip.setPixelColor(bucketMap[xPos][yPos], 0, 100, 0);
      }
    };
    frame = (frame + 1) % (X * rate);
  } else if (mode == 1) {
    if (lvl1 > lvl2) {
      if (lvl1 > 600) {
        lvl2 = 600;
      } else {
        lvl2 = lvl1;
      }
    }
    yPos = int(lvl2/75);
    for (xPos = 0; xPos < X; xPos++) {
      strip.setPixelColor(bucketMap[xPos][yPos],125,0,0);
      for (int blue = yPos - 1; blue >= 0; blue--) {
        strip.setPixelColor(bucketMap[xPos][blue], 0, 0, 125);
      }
    }
    lvl2 = lvl2 - 30;
  } else if (mode == 2) {
    int create = random(3);
    int newStars;
    if (create == 0) {
      newStars = random(1,3);
    } else {
      newStars = 0;
    }
    int createStars = newStars + ceil(lvl1/10);
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

