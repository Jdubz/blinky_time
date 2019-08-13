
typedef struct patternValues {
  uint8_t color;
  uint8_t frequency;
  uint8_t phase;
}
pattern;

pattern newPattern() {
  pattern patternValue;
  patternValue.color = random(255);
  patternValue.frequency = random(255);
  patternValue.phase = random(255);

  return patternValue;
}
