#pragma once
#include <Arduino.h>
#include <PDM.h>

class AdaptiveMic {
public:
  void  begin();
  void  update();

  // Levels (0..~1)
  float getRMS();         // software-gain normalized, smoothed (envelope-applied)
  float getPeak();        // raw peak (pre software gain)
  float getEnvelope();    // transient envelope * software gain

  // For debugging/tuning
  float getSoftwareGain();
  int   getHardwareGain();

private:
  static void onPDMdata();
  static volatile int16_t sampleBuffer[512];
  static volatile int     sampleBufferSize;

  // Level tracking
  float rms       = 0.0f;   // envelope * swGain
  float peak      = 0.0f;   // raw peak
  float avgRMS    = 0.0f;   // short-term RMS smoother (for sw gain control)
  float envelope  = 0.0f;   // transient envelope (attack/release)

  // Gains
  float swGain          = 1.0f;  // software gain (fast)
  unsigned long lastHW  = 0;
  int   hwGain          = 20;    // hardware gain (slow, PDM)
};
