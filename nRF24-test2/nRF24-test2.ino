#include <nRF24L01.h>
#include <RF24.h>
#include <SPI.h>

int CSNPIN = 7;
int CEPIN = 8;
int MOSIPIN = 11;
int MISOPIN = 12;
int SCKPIN = 13;

RF24 radio(CSNPIN, CEPIN);

const byte address[6] = "00125";

void setup() {
  Serial.begin(9600);
  radio.begin();
  radio.openReadingPipe(0, address);
  radio.setPALevel(RF24_PA_MIN);
  radio.startListening();
}

void loop() {
  if (radio.available()) {
    int msg[1];
    radio.read(msg, 1);
    if (msg[0] != 0) {
      Serial.println(msg[0]);
    }
    if (msg[0] == 27) {
      Serial.println("Twenty-seven!");
      delay(1000);
    }
  } else {
    // Serial.println("no-radio");
  }
}
