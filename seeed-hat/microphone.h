#ifndef Microphone_h
#define Microphone_h

#include <PDM.h>

// default PCM output frequency
static const int frequency = 16000;

class Microphone {
  public:
    Microphone() {
      max = 20.0;
      PDM.onReceive(this->onPDMdata);
      if (!PDM.begin(1, frequency)) {
        Serial.println("Failed to start PDM!");
      }
    }
    void update() {
      int now = analogRead(this->pin);
      if (now > this->high) {
        this->high = now;
      }
      if (now > this->max) {
        this->max = float(now);
      }
    }
    float read() {
      if (this->samplesRead) {
        // Print samples to the serial monitor or plotter
        for (int i = 0; i < this->samplesRead; i++) {
          Serial.println(this->sampleBuffer[i]);
        }
        // Clear the read count
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
    void onPDMdata() {
      // Query the number of available bytes
      int bytesAvailable = PDM.available();
    
      // Read into the sample buffer
      PDM.read(this->sampleBuffer, bytesAvailable);
    
      // 16-bit, 2 bytes per sample
      this->samplesRead = bytesAvailable / 2;
    }

  private:
    short sampleBuffer[512];
    volatile int samplesRead;
    int high;
    float max;

};

#endif
