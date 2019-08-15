#include "pendulum.h"
#include "color.h"
#include "pattern.h"
#include "button.h"
#include "radio.h"

Button* button = new Button(3);
Radio* antenna = new Radio();
pattern patternValue;

const int DELAYTIME = 30;

void render() {
  strip.show();
  for (int led = 0; led < NUMLEDS; led++) {
    strip.setPixelColor(led, 0, 0, 0);
  }
}

void setup() {
  Serial.begin(9600);
  strip.begin();
  strip.show();
  patternValue = newPattern();
  antenna.init();
}

void loop() {

  button->update();
  if (button->wasShortPressed()) {
    patternValue = newPattern();
  }

  pendulumStep(patternValue.color, patternValue.phase);
  render();
  patternValue.phase = phaseStep(patternValue.phase);
  
  antenna.send(patternValue);
  
  if (Radio.listen(DELAYTIME)) {
    patternValue = antenna.getNewPattern();
  }
};
