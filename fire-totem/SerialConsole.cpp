#include "SerialConsole.h"
void SerialConsole::begin(){ if(Serial) Serial.println(F("SerialConsole ready.")); }
void SerialConsole::service(){ (void)fire_; (void)maxRows_; (void)mic_; }
