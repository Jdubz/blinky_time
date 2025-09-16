#include "SerialConsole.h"

void SerialConsole::begin() {
  if (!Serial) return;
  Serial.println(F("SerialConsole ready."));
  // Commands intentionally omitted for compatibility.
}

void SerialConsole::service() {
  // Placeholder for future commands if you re-enable them.
  (void)fire_;
  (void)maxRows_;
  (void)mic_;
}
