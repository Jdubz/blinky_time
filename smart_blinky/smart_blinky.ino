#include "Button.h"
#include "LED.h"
#include "ROM.h"
#include "Routes.h"
#include "WifiManager.h"

ESP8266WebServer httpRestServer(HTTP_REST_PORT);

void setup() {
  restServerRouting();
  httpRestServer.begin();

}

void loop() {
  // put your main code here, to run repeatedly:

}
