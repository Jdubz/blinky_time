#include "Esp32PdmMic.h"

#ifdef BLINKY_PLATFORM_ESP32S3

// arduino-esp32 3.x: I2S library was replaced with ESP_I2S.
// Use I2S_MODE_PDM_RX with setPinsPdmRx() instead of the legacy
// setPins()/PDM_MONO_MODE API from 2.x.
#include <ESP_I2S.h>

static I2SClass i2s;

bool Esp32PdmMic::begin(int channels, long sampleRate) {
    // Configure PDM-RX pins: CLK=42, DATA=41
    i2s.setPinsPdmRx(PDM_CLK_PIN, PDM_DATA_PIN);
    return i2s.begin(I2S_MODE_PDM_RX, (uint32_t)sampleRate,
                     I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
}

void Esp32PdmMic::end() {
    i2s.end();
}

void Esp32PdmMic::setGain(int gain) {
    // No hardware gain register accessible via ESP_I2S PDM API on ESP32-S3.
    // AdaptiveMic tracks currentHardwareGain internally but hardware is unaffected.
    (void)gain;
}

void Esp32PdmMic::onReceive(ReceiveCallback callback) {
    // Store callback for later invocation from poll().
    // Unlike nRF52, we do NOT register a hardware interrupt here.
    callback_ = callback;
}

int Esp32PdmMic::available() {
    return stagingCount_;
}

int Esp32PdmMic::read(int16_t* buffer, int maxBytes) {
    int toCopy = maxBytes < (stagingCount_ * (int)sizeof(int16_t))
                 ? maxBytes
                 : stagingCount_ * (int)sizeof(int16_t);
    memcpy(buffer, staging_, toCopy);
    stagingCount_ = 0;
    return toCopy;
}

void Esp32PdmMic::poll() {
    // Drain however many bytes the I2S DMA has ready
    int bytesAvailable = i2s.available();
    if (bytesAvailable <= 0) return;

    // Cap to staging buffer capacity
    int maxBytes = STAGING_SIZE * (int)sizeof(int16_t);
    int bytesToRead = (bytesAvailable < maxBytes) ? bytesAvailable : maxBytes;

    int bytesRead = (int)i2s.readBytes((char*)staging_, (size_t)bytesToRead);
    stagingCount_ = bytesRead / (int)sizeof(int16_t);

    // Fire the callback (equivalent to the nRF52 hardware interrupt firing).
    // The callback (AdaptiveMic::onPDMdata) will call available() + read()
    // to consume from staging_.
    if (callback_ && stagingCount_ > 0) {
        callback_();
    }
}

#endif // BLINKY_PLATFORM_ESP32S3
