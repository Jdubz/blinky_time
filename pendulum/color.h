#ifndef Color_h
#define Color_h

const uint8_t brightness = 150;

typedef struct colorValues {
  uint8_t green;
  uint8_t red;
  uint8_t blue;
}
color;

color getSingleColorValue(uint8_t colorVal) {
  color colorValue;
  float ramp = (colorVal % 85) / 85;
  if (colorVal <= 85) {
    colorValue.green = int(ramp * brightness);
    colorValue.red = int((1.0 - ramp) * brightness);
    colorValue.blue = 0;
  } else if (colorVal <= 170) {
    colorValue.blue = int(ramp * brightness);
    colorValue.green = int((1.0 - ramp) * brightness);
    colorValue.red = 0;
  } else if (colorVal > 170) {
    colorValue.red = int(ramp * brightness);
    colorValue.blue = int((1.0 - ramp) * brightness);
    colorValue.green = 0;
  }

  return colorValue;
}

#endif
