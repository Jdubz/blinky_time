# blinky_time
Neopixel control from arduino nano, with microphone and wifi transiever

## Development Environment

1. Download and install the arduino IDE from [the arduino website](https://www.arduino.cc/en/Main/Software)
2. Download the Adafruit_Neopixel libary by going to `Sketch > Include Library > Manage Libraries...`. In the library manager search enter `Adafruit Neopixel` and highlight `Adafruit Neopixel` such that the version selection and install button becomes visible. Select `Version 1.1.2`, hit `Install` and close the Library Manager window when it completes.

## Uploading Builds

1. Attach the serial port to your build machine
2. Open the arduino IDE on your build machine with the basicString sketch loaded up
3. Select your board by going to `Tools > Board` and select `Arduino Nano`
4. Select your processor by going to `Tools > Processor` and select the `ATmega328P`
5. Select the appropriate port. Should be something like `/dev/tty.usb...`
6. In the main IDE, select the button that has an arrow on it. This is the upload button.

### To Do
```
- organize code
- document hardware
- open source in phases
```
