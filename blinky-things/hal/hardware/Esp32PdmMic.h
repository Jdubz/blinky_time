#pragma once
#include "../PlatformDetect.h"

#ifdef BLINKY_PLATFORM_ESP32S3

#include "../interfaces/IPdmMic.h"

/**
 * Esp32PdmMic - IPdmMic implementation for XIAO ESP32-S3 Sense
 *
 * Uses the raw ESP-IDF I2S PDM-RX driver (not the Arduino ESP_I2S wrapper).
 *
 * IMPORTANT — two ESP32-S3-specific issues handled in this driver:
 *
 *   1. JTAG pin conflict: GPIO42 (PDM CLK) = MTMS, GPIO41 (PDM DATA) = MTDI.
 *      When compiled with USBMode=hwcdc (required for serial on ESP32 core
 *      3.3.7+), the USB_SERIAL_JTAG peripheral may claim these pins at boot.
 *      begin() calls gpio_reset_pin() to release them before I2S init.
 *
 *   2. Partial DMA reads: ESP-IDF i2s_channel_read() returns ESP_ERR_TIMEOUT
 *      when the full requested size isn't read before timeout, but bytesRead
 *      can be >0 (partial DMA buffer). poll() checks only bytesRead, not err.
 *
 * Gain: No hardware PDM gain register on ESP32-S3. setGain() applies a software
 * linear multiplier to PCM samples in poll(). Does NOT improve SNR.
 *
 * Polling model (vs nRF52 interrupt model):
 *   poll() is called once per frame from AdaptiveMic::update(). It drains
 *   the I2S DMA ring buffer and fires the stored callback.
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

    // Hardware capability: ESP32-S3 has no PDM gain register — software gain only.
    // All four capability queries (hasHardwareGain=false, min=0, max=80, step=1.0)
    // match the IPdmMic defaults, so no overrides are needed here.

private:
    static constexpr int STAGING_SIZE = 512;   // samples (1024 bytes)
    ReceiveCallback callback_     = nullptr;
    int16_t         staging_[STAGING_SIZE];
    int             stagingCount_ = 0;
    float           softwareGain_ = 1.0f;      // linear; set by setGain()
};

#endif // BLINKY_PLATFORM_ESP32S3
