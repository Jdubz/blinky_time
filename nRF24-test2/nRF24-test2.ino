#include <nRF24L01.h>
#include <RF24.h>
#include <SPI.h>

int CSNPIN = 9;
int CEPIN = 10;
int MOSIPIN = 11;
int MISOPIN = 12;
int SCKPIN = 13;

int KNOBPIN = 14;
int LEDPIN = 3;

RF24 radio(CSNPIN, CEPIN);

const uint64_t address1 = 0xE8E8F0F0E1LL;
const uint64_t address2 = 0xE6E6E6E6E6E6;

int newKnob = 0;
int oldKnob = 0;

bool readKnob() {
  int reading = 0;
  for (int val = 0; val < 10; val++) {
    reading = reading + analogRead(KNOBPIN);
    newKnob = reading / 10;
  }
  if (newKnob > oldKnob + 5 || newKnob < oldKnob - 5) {
    oldKnob = newKnob;
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(9600);
  radio.begin();
  radio.openWritingPipe(address1);
  radio.openReadingPipe(1, address2);
  radio.setPALevel(RF24_PA_MIN);

  pinMode(LEDPIN, OUTPUT);
}

void loop() {
  delay(5);
  if (radio.available()) {
    radio.startListening();
    int knobIn = 0;
    radio.read(&knobIn, sizeof(knobIn));
    if (knobIn != -1) {
      Serial.print("radio in : ");
      Serial.println(knobIn);
      analogWrite(LEDPIN, knobIn / 4);
    }

    delay(5);
    if (readKnob()) {
      radio.stopListening();
      Serial.print("knob read : ");
      Serial.println(newKnob / 4);
      analogWrite(LEDPIN, newKnob / 4);
      radio.write(&newKnob, sizeof(newKnob));
    }

  } else {
    // Serial.println("no-radio");
  }
}
