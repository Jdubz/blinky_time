#include <Adafruit_NeoPixel.h>

#include "types.h"
#include "radio.h"
#include "button.h"
#include "microphone.h"
#include "keepalive.h"
#include "sparks.h"
#include "timer.h"
#include "radio.h"

#define LED_PIN     2
#define BUTTON_PIN  16
#define MIC_PIN     A0
#define PULL_PIN    5

#define NUM_LEDS    144

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

Button* button = new Button(BUTTON_PIN);
Microphone* mic = new Microphone(MIC_PIN);
// KeepAlive* keepAlive = new KeepAlive(PULL_PIN);
WifiServer* wifiServer = new WifiServer();

Timer* renderTimer = new Timer(30);
// Timer* pullTimer = new Timer(10000);

Sparks* sparks = new Sparks(NUM_LEDS);

color frame[NUM_LEDS];
void initFrame() {
  for (int i = 0; i < NUM_LEDS; i++) {
    color empty = { 0, 0, 0 };
    frame[i] = empty;
  }
}

void clear() {
  for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
}

void startup() {
  strip.begin();
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
  clear();
  strip.show();
  delay(500);
}

void render() {
  for (int led = 0; led < NUM_LEDS; led++) {
    strip.setPixelColor(led, frame[led].red, frame[led].green, frame[led].blue);
  }
  strip.show();
  clear();
}

void update() {
  button->update();
  mic->update();
  // keepAlive->pullKey(!pullTimer->trigger());
}

void setup() {
  Serial.begin(115200);
  startup();
  initFrame();
  wifiServer->startEsp();
}

void loop() {
  update();  
  if (button->wasShortPressed()) {
    Serial.print("short press");
    Serial.println();
  }

  if (renderTimer->trigger()) {
    float micLvl = mic->read();
    mic->attenuate();
    sparks->run(frame, micLvl);
    render();
    wifiServer->send("update", micLvl);
  }
}
