// For Debugging
//  1. #include <printf.h>
//  2. Add `printf_begin();` after Serial.begin(...
//  3. Dump radio debug info with radio.printDetails()

#include <SPI.h>
#include "RF24.h"

int LEDPIN = 3;
int KNOBPIN = 14;

RF24 radio(9,10); // CE, CSN

// Pipes 2-5 only look at the first byte
// try to only make the first byte unique
const byte address[6] = "10000";

const uint8_t pipeNum = 0;

// Used to control whether this node is sending or receiving
// 1 for sender, 0 for receiver
bool role = 0;

void setup() {
  Serial.begin(115200);
  pinMode(LEDPIN, OUTPUT);
  radio.begin();
  radio.setPALevel(RF24_PA_MIN);
  
  // Open a writing and reading pipe on each radio, with opposite addresses
  if(role){
    Serial.println("Initializing sender");
    radio.openWritingPipe(address);
    radio.stopListening();
  } else {
    Serial.println("Initializing receiver");
    radio.openReadingPipe(pipeNum, address);
    radio.startListening();
  }
}

void loop() {
  if (role == 1)  {
    // Sender
    analogWrite(LEDPIN, 50);

    const int code = analogRead(KNOBPIN);
    Serial.print("Sending code: ");
    Serial.println(code);
    
    // TODO add timeout if write takes too long
    radio.write(&code, sizeof(code));
    analogWrite(LEDPIN, 0);
    delay(1000);
  } else if ( role == 0 ) {
    // Receiver
    if( radio.available(&pipeNum)){
      int code = 0;
      radio.read(&code, sizeof(code));

      if (code == 482) {
        Serial.print("Correct code received: ");
        Serial.println(code);
        analogWrite(LEDPIN, 50);
        delay(1000);
        analogWrite(LEDPIN, 0);
      } else {
        Serial.print("Incorrect received: ");
        Serial.println(code);
        delay(100);
      }
    }
  }

  // Respond to serial commands
//  if ( Serial.available() ) {
//    char c = toupper(Serial.read());
//    Serial.print("Received serial communication: ");
//    Serial.println(c);
//    
//    if ( c == 'T' && role == 0 ) {      
//      Serial.println(F("*** CHANGING TO TRANSMIT ROLE -- PRESS 'R' TO SWITCH BACK"));
//      role = 1;                  // Become the primary transmitter (ping out)
//    
//    } else if ( c == 'R' && role == 1 ) {
//      Serial.println(F("*** CHANGING TO RECEIVE ROLE -- PRESS 'T' TO SWITCH BACK"));      
//      role = 0;                // Become the primary receiver (pong back)
//      radio.startListening();
//    }
//  }


} // Loop
