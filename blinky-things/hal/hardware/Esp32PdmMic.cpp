#include "Esp32PdmMic.h"

#ifdef BLINKY_PLATFORM_ESP32S3

#include <driver/i2s_pdm.h>
#include <driver/gpio.h>
#include <math.h>
#include <string.h>

static i2s_chan_handle_t rx_handle = nullptr;

bool Esp32PdmMic::begin(int channels, long sampleRate) {
    (void)channels;  // always mono

    // CRITICAL: Release PDM pins from JTAG before I2S init.
    //
    // On XIAO ESP32-S3 Sense, the PDM microphone is wired to GPIO42 (CLK) and
    // GPIO41 (DATA). These are also the JTAG strap pins (MTMS and MTDI).
    // When compiled with USBMode=hwcdc (usb_mode=1, required for serial on this
    // board with ESP32 core 3.3.7+), the USB_SERIAL_JTAG peripheral may claim
    // these pins via IO_MUX during boot. The I2S driver's gpio_set_direction()
    // silently fails to override the JTAG mux, so i2s_channel_read() returns
    // zero bytes forever — the mic appears dead while begin() returns true.
    //
    // gpio_reset_pin() disconnects the pin from any peripheral (including JTAG),
    // resets it to GPIO function, and sets it as input. The I2S PDM init that
    // follows will then successfully claim the pins for PDM CLK/DATA.
    gpio_reset_pin((gpio_num_t)PDM_CLK_PIN);
    gpio_reset_pin((gpio_num_t)PDM_DATA_PIN);

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

    // Verify data is actually flowing. The DMA fills one buffer every ~15ms
    // (240 samples at 16 kHz). If no data arrives within 100ms, the pin
    // release above didn't work or the mic hardware is absent.
    int16_t verifyBuf[64];
    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(rx_handle, verifyBuf, sizeof(verifyBuf),
                                      &bytesRead, 100);
    if (bytesRead == 0) {
        i2s_channel_disable(rx_handle);
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
    int availableBytes = stagingCount_ * (int)sizeof(int16_t);
    int toCopy = (maxBytes < availableBytes) ? maxBytes : availableBytes;
    memcpy(buffer, staging_, toCopy);

    // Only consume the samples that were actually copied.
    // If maxBytes was smaller than available, shift the remainder.
    int samplesCopied = toCopy / (int)sizeof(int16_t);
    int remaining = stagingCount_ - samplesCopied;
    if (remaining > 0) {
        memmove(staging_, staging_ + samplesCopied, remaining * sizeof(int16_t));
    }
    stagingCount_ = remaining;
    return toCopy;
}

void Esp32PdmMic::poll() {
    if (!rx_handle) return;

    // Use 1ms timeout instead of 0. ESP-IDF v5.x i2s_channel_read() with
    // timeout_ms=0 unreliably returns ESP_ERR_TIMEOUT even when DMA buffers
    // are ready (FreeRTOS tick resolution issue). 1ms adds negligible latency
    // at 60 Hz frame rate and catches data reliably.
    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(rx_handle, staging_,
                                     STAGING_SIZE * sizeof(int16_t),
                                     &bytesRead, 1);
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
