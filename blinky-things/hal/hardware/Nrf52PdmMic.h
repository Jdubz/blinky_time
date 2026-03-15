#pragma once
#include "../PlatformDetect.h"

#ifdef BLINKY_PLATFORM_NRF52840

#include "../interfaces/IPdmMic.h"

// NOTE: PDM.h is NOT included here to avoid pinDefinitions.h redefinition
// bug in Seeeduino mbed platform. Implementation is in Nrf52PdmMic.cpp.

/**
 * Nrf52PdmMic - IPdmMic implementation for nRF52840 using PDM library
 *
 * nRF52840 PDM hardware provides true pre-decimation gain via GAINL/GAINR
 * registers (range 0–80 in 0.5 dB steps), which improves SNR.
 */
class Nrf52PdmMic : public IPdmMic {
public:
    bool begin(int channels, long sampleRate) override;
    void end() override;
    void setGain(int gain) override;
    void onReceive(ReceiveCallback callback) override;
    int available() override;
    int read(int16_t* buffer, int maxBytes) override;

    // Hardware capability: nRF52840 has real PDM gain registers
    bool  hasHardwareGain() const override { return true; }
    int   getGainMinDb()    const override { return 0; }
    int   getGainMaxDb()    const override { return 80; }
    float getGainStepDb()   const override { return 0.5f; }
};

#endif // BLINKY_PLATFORM_NRF52840
