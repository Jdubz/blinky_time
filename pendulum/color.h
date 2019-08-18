#ifndef Color_h
#define Color_h

typedef struct colorValues {
  uint8_t green;
  uint8_t red;
  uint8_t blue;
}
color;

color getSingleColorValue(uint8_t colorVal) {
  color colorValue;
  float ramp = float(colorVal % 85) / 85.0;
  if (colorVal <= 85) {
    colorValue.green = int(ramp * 255);
    colorValue.red = int((1.0 - ramp) * 255);
    colorValue.blue = 0;
  } else if (colorVal <= 170) {
    colorValue.blue = int(ramp * 255);
    colorValue.green = int((1.0 - ramp) * 255);
    colorValue.red = 0;
  } else if (colorVal > 170) {
    colorValue.red = int(ramp * 255);
    colorValue.blue = int((1.0 - ramp) * 255);
    colorValue.green = 0;
  }

  return colorValue;
}

color getFlippedColorOf(color referenceColor) {
  color flipped;
  flipped.green = (referenceColor.green + 125) % 255;
  flipped.red = (referenceColor.red + 125) % 255;
  flipped.blue = (referenceColor.blue + 125) % 255;

  return flipped;
}

color calculateSwing(float height, color color1, color color2) {
  color swungColor;
  float amplitude;
  if (height <= 0.5) {
    amplitude = (0.5 - height) * 2.0;
    swungColor.green = color1.green * amplitude;
    swungColor.red = color1.red * amplitude;
    swungColor.blue = color1.blue * amplitude;
  } else {
    amplitude = (height - 0.5) * 2.0;
    swungColor.green = color2.green * amplitude;
    swungColor.red = color2.red * amplitude;
    swungColor.blue = color2.blue * amplitude;
  }

  return swungColor;
}

#endif
