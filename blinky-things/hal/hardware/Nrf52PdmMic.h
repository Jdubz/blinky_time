#pragma once
#include "../interfaces/IPdmMic.h"

// NOTE: PDM.h is NOT included here to avoid pinDefinitions.h redefinition
// bug in Seeeduino mbed platform. Implementation is in Nrf52PdmMic.cpp.

/**
 * Nrf52PdmMic - IPdmMic implementation for nRF52840 using PDM library
 */
class Nrf52PdmMic : public IPdmMic {
public:
    bool begin(int channels, long sampleRate) override;
    void end() override;
    void setGain(int gain) override;
    void onReceive(ReceiveCallback callback) override;
    int available() override;
    int read(int16_t* buffer, int maxBytes) override;
};
