/**
 * XIAO nRF52840 Recovery Sketch
 * 
 * This is the most basic sketch possible to recover a crashed XIAO nRF52840.
 * It does minimal initialization and just blinks the built-in LED.
 */

void setup() {
  // Initialize built-in LED (pin 11 on XIAO nRF52840)
  pinMode(11, OUTPUT);
  
  // Simple startup indication - fast blinks
  for(int i = 0; i < 10; i++) {
    digitalWrite(11, HIGH);
    delay(100);
    digitalWrite(11, LOW);
    delay(100);
  }
}

void loop() {
  // Just blink the built-in LED slowly to show it's alive
  digitalWrite(11, HIGH);
  delay(1000);
  digitalWrite(11, LOW);
  delay(1000);
}