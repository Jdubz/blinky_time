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
      gain = 20;
      sampleIndex = 0;
      PDM.onReceive(onPDMdata);
      if (!PDM.begin(1, frequency)) {
        Serial.println("Failed to start PDM!");
      }
    }
    float read() {
      // max mic read = 32768.00

      if (samplesRead) {
        float high = 0.0;
        for (int i = 0; i < samplesRead; i++) {
          float sample = abs(sampleBuffer[i]);

          if (sample > high) {
            high = sample;
          }
        }
        this->attenuate(high);
        
        samplesRead = 0;
        if (high > this->max) {
          this->max = high;
        }
        Serial.println(this->max);
        return float(high) / this->max;
      }
      return 0.0;
    }
    void attenuate(float sample) {
      float decay = 10.0;
      if (this->max >= 10000.0) {
        this->max -= decay;
      }

      this->samples[this->sampleIndex] = sample;
      this->sampleIndex++;

      if (this->sampleIndex == 50) {
        float sum = 0.0;
        for (int i = 0; i < 50; i++) {
          sum += this->samples[i];
        }
        float avg = sum / 50;
        if (avg > 15000.0 && this->gain > 1) {
          this->gain--;
        } else if (avg <= 15000.0 && this->gain < 80) {
          this->gain++;
        }

        PDM.setGain(this->gain);
        this->sampleIndex = 0;
      }
    }

  private:
    float max;
    int gain;
    int sampleIndex;
    float samples[50];
};

#endif
