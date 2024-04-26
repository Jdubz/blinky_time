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
      max = 20.0;
      PDM.onReceive(onPDMdata);
      if (!PDM.begin(1, frequency)) {
        Serial.println("Failed to start PDM!");
      }
    }
    float read() {
      if (samplesRead) {
        int high = 0;
        int low = 1024;
        for (int i = 0; i < samplesRead; i++) {
          int sample = abs(sampleBuffer[i]);
          if (sample < low) {
            low = sample;
          }
          if (sample > high) {
            high = sample;
          }
        }
        Serial.println(high, low, high-low);
        samplesRead = 0;
      }
      int sample = this->high;
      this->high = 0;
      return float(sample) / this->max;
    }
    void attenuate() {
      float decay = 0.25;
      if (this->max >= 20.0) {
        this->max -= decay;
      }
    }

  private:
    int high;
    float max;

};

#endif
