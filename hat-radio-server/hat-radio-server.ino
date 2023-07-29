
#include "radio.h"
#include "button.h"
#include "microphone.h"
#include "leds.h"
#include "keepalive.h"
#include "sparks.h"
#include "timer.h"
#include "types.h"

#define LED_PIN     2
#define BUTTON_PIN  16
#define MIC_PIN     A0
#define PULL_PIN    5

#define NUM_LEDS    72

Button* button = new Button(BUTTON_PIN);
Microphone* mic = new Microphone(MIC_PIN);
Leds* leds = new Leds(LED_PIN, NUM_LEDS);
KeepAlive* keepAlive = new KeepAlive(PULL_PIN);
Timer* timer = new Timer();

Sparks* sparks = new Sparks();

color empty;
empty.red = 0;
empty.green = 0;
empty.blue = 0;
color frame[NUM_LEDS] = { empty };

void update() {
  keepAlive->pullKey();
  button->update();
  mic->update();
}

void setup() {
  Serial.begin(9600);
  keepAlive->start();
  leds->startup();
}

void loop() {
  update();
  if (button->wasShortPressed()) {
    Serial.print("short press");
    Serial.println();
  }

  if (timer->render()) {
    float micLvl = mic->read();
    mic->attenuate();
  }
}
