#ifndef Button_h
#define Button_h

// TODO refactor to make presses feel more immediate
class Button {
  public:
    Button(int inputPin) {
      pin = inputPin;
      pinMode(inputPin, INPUT);
      wasDown = false;
      wasPressed = false;
      longPressDuration = 1500;
      pressDuration = -1;
      isLongPressed = false;
    }
    void update() {
      bool isDown = digitalRead(this->pin) == HIGH;
      bool isUp = digitalRead(this->pin) == LOW;

      if (isUp) {
        this->isLongPressed = false;
      }

      if (isDown && !this->wasDown) {
        this->wasDown = true;
        this->pressStart = millis();
      } else if (isUp && this->wasDown) {
        this->pressDuration = millis() - this->pressStart;
        this->wasDown = false;
        this->wasPressed = true;
      } else if (isDown && this->wasDown) {
        this->pressDuration = millis() - this->pressStart;
      } else {
        this->wasPressed = false;
      }
    }
    bool wasShortPressed() {
      return this->wasPressed && this->pressDuration < this->longPressDuration;
    }
    bool wasLongPressed() {
      bool longPressed = this->wasDown && this->pressDuration >= this->longPressDuration && !this->isLongPressed;
      if (longPressed) {
        this->isLongPressed = true;
      }
      return longPressed;
    }
  private:
    int pin;
    bool wasDown;
    bool wasPressed;
    float pressStart;
    float longPressDuration;
    float pressDuration;
    bool isLongPressed;
};

#endif
