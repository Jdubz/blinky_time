#include "Button.h"
#include "LED.h"
#include "ROM.h"
#include "Routes.h"
#include "WifiManager.h"

#define ButtonPin D3
#define LEDPin D4
#define Rpin D5
#define Gpin D6
#define Bpin D7

Button button = Button(ButtonPin);
LED led = LED(LEDPin);
ROM rom = ROM();
Light light = Light(Rpin, Gpin, Bpin);
WifiManager wifi = WifiManager(led);

void setup() {
  
  wifi.connect();

}

void loop() {
  // put your main code here, to run repeatedly:

}
