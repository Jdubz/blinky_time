#include "Esp32PdmMic.h"

#ifdef BLINKY_PLATFORM_ESP32S3

#include <driver/i2s_pdm.h>
#include <math.h>
#include <string.h>

static i2s_chan_handle_t rx_handle = nullptr;

bool Esp32PdmMic::begin(int channels, long sampleRate) {
    (void)channels;  // always mono

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    if (i2s_new_channel(&chan_cfg, NULL, &rx_handle) != ESP_OK) return false;

    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)sampleRate),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk          = (gpio_num_t)PDM_CLK_PIN,
            .din          = (gpio_num_t)PDM_DATA_PIN,
            .invert_flags = { .clk_inv = false },
        },
    };

    if (i2s_channel_init_pdm_rx_mode(rx_handle, &pdm_cfg) != ESP_OK) {
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
        return false;
    }
    if (i2s_channel_enable(rx_handle) != ESP_OK) {
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
        return false;
    }
    return true;
}

void Esp32PdmMic::end() {
    if (rx_handle) {
        i2s_channel_disable(rx_handle);
        i2s_del_channel(rx_handle);
        rx_handle = nullptr;
    }
}

void Esp32PdmMic::setGain(int gainDb) {
    // ESP32-S3 has no hardware PDM gain register — full range is software only.
    // gainDb == 0  →  1.0x linear (no scaling needed, poll() skips the multiply).
    // gainDb > 0   →  amplify post-decimation (does not improve SNR).
    // Negative dB would attenuate; AGC never goes below getGainMinDb()=0 so
    // this branch is unreachable in normal use.
    softwareGain_ = (gainDb > 0) ? powf(10.0f, gainDb / 20.0f) : 1.0f;
}

void Esp32PdmMic::onReceive(ReceiveCallback callback) {
    callback_ = callback;
}

int Esp32PdmMic::available() {
    // Return bytes (not samples) to match the IPdmMic contract used by onPDMdata()
    return stagingCount_ * (int)sizeof(int16_t);
}

int Esp32PdmMic::read(int16_t* buffer, int maxBytes) {
    int toCopy = (maxBytes < stagingCount_ * (int)sizeof(int16_t))
                 ? maxBytes
                 : stagingCount_ * (int)sizeof(int16_t);
    memcpy(buffer, staging_, toCopy);
    stagingCount_ = 0;
    return toCopy;
}

void Esp32PdmMic::poll() {
    if (!rx_handle) return;

    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(rx_handle, staging_,
                                     STAGING_SIZE * sizeof(int16_t),
                                     &bytesRead, 0);
    if (err != ESP_OK || bytesRead == 0) return;

    stagingCount_ = (int)bytesRead / (int)sizeof(int16_t);

    // Software gain (covers full AGC range — no hardware gain available on ESP32-S3)
    if (softwareGain_ > 1.01f) {
        for (int i = 0; i < stagingCount_; i++) {
            float s = staging_[i] * softwareGain_;
            if      (s >  32767.0f) staging_[i] =  32767;
            else if (s < -32768.0f) staging_[i] = -32768;
            else                    staging_[i]  = (int16_t)s;
        }
    }

    if (callback_) callback_();
}

#endif // BLINKY_PLATFORM_ESP32S3
