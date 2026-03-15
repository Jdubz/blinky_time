#pragma once
#include "../PlatformDetect.h"

#ifdef BLINKY_PLATFORM_ESP32S3

#include "../interfaces/IPdmMic.h"

/**
 * Esp32PdmMic - IPdmMic implementation for XIAO ESP32-S3 Sense
 *
 * Uses the ESP32 Arduino I2S library in PDM-RX mode.
 * Hardware: MSM381ACT PDM microphone, CLK=GPIO42, DATA=GPIO41
 *
 * Requires: arduino-esp32 >= 3.0 (uses ESP_I2S library, not legacy I2S.h)
 *
 * Polling model (vs nRF52 interrupt model):
 *   nRF52: PDM hardware interrupt fires onReceive callback automatically.
 *   ESP32: No hardware PDM interrupt available via Arduino I2S API.
 *          poll() is called once per frame from AdaptiveMic::update().
 *          It drains the I2S DMA buffer into a staging area and fires
 *          the stored callback, making the rest of AdaptiveMic unchanged.
 *
 * Hardware gain:
 *   setGain() is a no-op — ESP32-S3 I2S PDM has no accessible hardware
 *   gain register. AdaptiveMic's software AGC tracks gain state internally
 *   but hardware output is unaffected. Audio levels will be managed by the
 *   software normalization pipeline in AdaptiveMic.
 */
class Esp32PdmMic : public IPdmMic {
public:
    static constexpr int PDM_CLK_PIN  = 42;  // GPIO42 on XIAO ESP32-S3 Sense
    static constexpr int PDM_DATA_PIN = 41;  // GPIO41 on XIAO ESP32-S3 Sense

    bool begin(int channels, long sampleRate) override;
    void end() override;
    void setGain(int gain) override;
    void onReceive(ReceiveCallback callback) override;
    int available() override;
    int read(int16_t* buffer, int maxBytes) override;
    void poll() override;

private:
    static constexpr int STAGING_SIZE = 512;  // samples (1024 bytes)
    ReceiveCallback callback_ = nullptr;
    int16_t staging_[STAGING_SIZE];
    int stagingCount_ = 0;
};

#endif // BLINKY_PLATFORM_ESP32S3
