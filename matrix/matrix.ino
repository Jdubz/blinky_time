#include <Adafruit_NeoPixel.h>
int LEDPIN = 2;
const int LEDS = 256;
int frameRate = 30;
int frame = 0;

int rcNum(int R, int C) {
  if (R % 2) {
    return R*16 + (15 - C);
  }
  return (R*16) + C;
}

Adafruit_NeoPixel strip = Adafruit_NeoPixel(LEDS, LEDPIN, NEO_GRB + NEO_KHZ800);

void renderStrip() {
  strip.show();
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
  delay(frameRate);
}

void setup() {
  Serial.begin(9600);
  
  strip.begin();
  strip.show();

  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 10, 0, 0);
  }
  strip.show();
  delay(300);
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 10, 0);
  }
  strip.show();
  delay(300);
  for (int led = 0; led < LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 10);
  }
  strip.show();
  delay(300);
  renderStrip();
  strip.show();
}

// With `port.write('1,100,80,60');` in the node code

void loop() {
  String buf = "";
  char current = 'c';

  if(Serial.available()) {
    current = Serial.read();
    buf = buf.concat(current);
//      char ledNum = Serial.parseInt();
//      char red = Serial.parseInt();
//      char green = Serial.parseInt();
//      char blue = Serial.parseInt();
//      Serial.flush();
//
//      Serial.println(ledNum);
//      Serial.println(red);
//      Serial.println(green);
//      Serial.println(blue);
  }
 

  Serial.println(buf);
 
//  if (Serial.available()) {
//    incomingByte = Serial.read();
//    Serial.println(incomingByte, DEC);
//    Serial.flush();
    
//    byte read[768];
//    Serial.readBytes(read, sizeof(read));
//    Serial.println(sizeof(read));
//    int nextPixel[3]; 
//    for (int b = 0; b < sizeof(read); b++) {
//      nextPixel[b%3] = read[b];
//      if (b%3 == 2) {
//        strip.setPixelColor(floor(b/3), nextPixel[0]/10, nextPixel[1]/10, nextPixel[2]/10);
//      }
//    }
//
//    for (int i = 0; i < sizeof(read); i++) {
//      Serial.print(String(read[i]));
//    }
//    Serial.println();
//    renderStrip();
//  }
//
//  frame++;
//
//  if (frame == frameRate * 100) {
//    renderStrip();
//    frame = 0;
//  }
}
