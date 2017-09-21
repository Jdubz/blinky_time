#include <Adafruit_NeoPixel.h>

int LEDPIN = 6;
int MICPIN = A0; // 0 - 600
int BUTTONPIN = 4;
int NUMLEDS = 112;
int X = 14;
int Y = 8;
int DELAYTIME = 30;
int NUMMODES = 5;

int mode = 3;
int pressed = 0;

int xPos = 0;
int yPos = 0;
int lvl2 = 0;
int frame = 0;

Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUMLEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

int bucketMap[14][8] = {
  {0,27,28,55,56,83,84,111},
  {1,26,29,54,57,82,85,110},
  {2,25,30,53,58,81,86,109},
  {3,24,31,52,59,80,87,108},
  {4,23,32,51,60,79,88,107},
  {5,22,33,50,61,78,89,106},
  {6,21,34,49,62,77,90,105},
  {7,20,35,48,63,76,91,104},
  {8,19,36,47,64,75,92,103},
  {9,18,37,46,65,74,93,102},
  {10,17,38,45,66,73,94,101},
  {11,16,39,44,67,72,95,100},
  {12,15,40,43,68,71,96,99},
  {13,14,41,42,69,70,97,98},
};

int fireMap[14][2];
int stars[112];
int snakes[3][7][2] = {
  {{1,3},{1,3},{1,3},{1,3},{1,3},{1,3},{1,3}},
  {{4,4},{4,4},{4,4},{4,4},{4,4},{4,4},{4,4}},
  {{8,5},{8,5},{8,5},{8,5},{8,5},{8,5},{8,5}},
};

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
  } else if (mode == 3) {
    int newFire = 1 + ceil(lvl1/30);
    for (newFire; newFire > 0; newFire--) {
      fireMap[random(X)][0] = random(Y/2 + newFire);
    }
    for (xPos = 0; xPos < X; xPos++) {
      int yLvl = (fireMap[xPos][1] * 10);
      for (yPos = fireMap[xPos][1]; yPos >= 0; yPos--) {
        strip.setPixelColor(bucketMap[xPos][yPos], yLvl, 100, 0);
        yLvl = yLvl - 10;
      }
      if (fireMap[xPos][0] > 1 && fireMap[xPos][1] < Y) {
        fireMap[xPos][1] = fireMap[xPos][1] + 1;
      }
      if (fireMap[xPos][0] > 1) {
        fireMap[xPos][0] = fireMap[xPos][0] - 1;
      } else {
        fireMap[xPos][1] = 0;
      }
    }
  } else if (mode == 4) {
    for (int s = 2; s >= 0; s--) {
      if (frame % 3 == 0) {
        int dirX = random(3) - 1;
        int dirY = random(3) - 1;
        int nextX = snakes[s][0][0] + dirX;
        int nextY = snakes[s][0][1] + dirY;
        if (nextX == X) {
          nextX = 0;
        } else if (nextX < 0) {
          nextX = X - 1;
        }
        if (nextY < 0) {
          nextY = 0;
        } else if (nextY == Y) {
          nextY = Y - 1;
        }
        for (int l = 6; l > 0; l--) {
          snakes[s][l][0] = snakes[s][l - 1][0];
          snakes[s][l][1] = snakes[s][l - 1][1];
        }
        snakes[s][0][0] = nextX;
        snakes[s][0][1] = nextY;
      }
      for (int sn = 0; sn < 7; sn++){
        if (s == 0) {
          strip.setPixelColor(bucketMap[snakes[s][sn][0]][snakes[s][sn][1]], 0, (80 - (sn * 10)), 20);
        } else if (s == 1) {
          strip.setPixelColor(bucketMap[snakes[s][sn][0]][snakes[s][sn][1]], (80 - (sn * 10)), 20, 0);
        } else {
          strip.setPixelColor(bucketMap[snakes[s][sn][0]][snakes[s][sn][1]], 20, 0, (80 - (sn * 10)));
        }
      }
    }
    frame = (frame + 1) % 3;
  }
};

