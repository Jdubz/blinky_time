#pragma once
#include "../PlatformDetect.h"

#ifdef BLINKY_PLATFORM_ESP32S3

#include "../interfaces/IPdmMic.h"

/**
 * Esp32PdmMic - IPdmMic implementation for XIAO ESP32-S3 Sense
 *
 * Uses the raw ESP-IDF I2S PDM-RX driver (not the Arduino ESP_I2S wrapper).
 *
 * Gain: The ESP32-S3 I2S PDM-RX hardware has no accessible gain register
 * (amplify_num, sd_scale, hp_scale are all absent from its slot config struct —
 * those fields exist only on other ESP32 variants). setGain() applies a software
 * linear multiplier to the PCM samples in poll(), covering the full AGC range.
 * This does NOT improve SNR — it is post-decimation amplitude scaling only.
 *
 * Polling model (vs nRF52 interrupt model):
 *   poll() is called once per frame from AdaptiveMic::update(). It drains
 *   the I2S DMA ring buffer and fires the stored callback, keeping the rest
 *   of the AdaptiveMic pipeline unchanged.
 *
 * Hardware: MSM381ACT PDM mic, CLK=GPIO42, DATA=GPIO41
 */
class Esp32PdmMic : public IPdmMic {
public:
    static constexpr int PDM_CLK_PIN  = 42;
    static constexpr int PDM_DATA_PIN = 41;

    bool begin(int channels, long sampleRate) override;
    void end() override;
    void setGain(int gainDb) override;
    void onReceive(ReceiveCallback callback) override;
    int  available() override;
    int  read(int16_t* buffer, int maxBytes) override;
    void poll() override;

    // Hardware capability: ESP32-S3 has no PDM gain register — software gain only
    bool  hasHardwareGain() const override { return false; }
    int   getGainMinDb()    const override { return 0; }
    int   getGainMaxDb()    const override { return 80; }
    float getGainStepDb()   const override { return 1.0f; }

private:
    static constexpr int STAGING_SIZE = 512;   // samples (1024 bytes)
    ReceiveCallback callback_     = nullptr;
    int16_t         staging_[STAGING_SIZE];
    int             stagingCount_ = 0;
    float           softwareGain_ = 1.0f;      // linear; set by setGain()
};

#endif // BLINKY_PLATFORM_ESP32S3
