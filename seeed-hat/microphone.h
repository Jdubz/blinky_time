#ifndef Microphone_h
#define Microphone_h

#include <PDM.h>

// default PCM output frequency
static const int frequency = 16000;

short sampleBuffer[512];
volatile int samplesRead;

void onPDMdata() {
  // Query the number of available bytes
  int bytesAvailable = PDM.available();

  // Read into the sample buffer
  PDM.read(sampleBuffer, bytesAvailable);

  // 16-bit, 2 bytes per sample
  samplesRead = bytesAvailable / 2;
}

class Microphone {
  public:
    Microphone() {
      max = 1000.0;
      PDM.onReceive(onPDMdata);
      //default 20 max 80
      //PDM.setGain(30);
      if (!PDM.begin(1, frequency)) {
        Serial.println("Failed to start PDM!");
      }
    }
    float read() {
      if (samplesRead) {
        float high = 0.0;
        for (int i = 0; i < samplesRead; i++) {
          float sample = abs(sampleBuffer[i]);
          if (sample > high) {
            high = sample;
          }
        }
        
        samplesRead = 0;
        if (high > this->max) {
          this->max = high;
        }
        return float(high) / this->max;
      }
      return 0.0;
    }
    void attenuate() {
      float decay = 5.0;
      if (this->max >= 1000.0) {
        this->max -= decay;
      }
    }

  private:
    float max;

};

#endif
