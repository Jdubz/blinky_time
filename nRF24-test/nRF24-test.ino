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

const uint64_t address2 = 0xE8E8F0F0E1LL;
const uint64_t address1 = 0xE6E6E6E6E6E6;
bool listening = false;

int knob = 0;
bool newKnob = false;

void readKnob() {
  int reading = 0;
  int knobVal = 0;
  for (int val = 0; val < 10; val++) {
    reading = reading + analogRead(KNOBPIN);
  }
  knobVal = reading / 10;
  if (knobVal > knob + 5 || knobVal < knob - 5) {
    knob = knobVal;
    newKnob = true;
    Serial.print("knob read : ");
    Serial.println(knobVal / 4);
  }
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
  // delay(5);
  readKnob();
  if (!listening) {
    radio.startListening();
    listening = true;
  }
  
  if (radio.available()) {
    int knobIn = 0;
    radio.read(&knobIn, sizeof(knobIn));
    if (knobIn > 0) {
      Serial.print("radio in : ");
      Serial.println(knobIn);
      analogWrite(LEDPIN, knobIn / 4);
    }
  } else {
    // Serial.println("no-radio");
  }
  
  if (newKnob) {
    radio.stopListening();
    analogWrite(LEDPIN, knob / 4);
    radio.write(&knob, sizeof(knob));
    // delay(5);
    listening = false;
    newKnob = false;
  }
}
