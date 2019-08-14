#ifndef PatternValues_h
#define PatternValues_h

#include "pendulum.h"

unsigned long cycleLength = getCycleLength();

typedef struct patternValues {
  uint8_t color;
  unsigned long phase;
}
pattern;

pattern newPattern() {
  pattern patternValue;
  patternValue.color = random(255);
  patternValue.phase = random(cycleLength);

  return patternValue;
}

#endif
