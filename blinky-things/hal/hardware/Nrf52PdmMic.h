#pragma once
#include "../interfaces/IPdmMic.h"
#include <PDM.h>

/**
 * Nrf52PdmMic - IPdmMic implementation for nRF52840 using PDM library
 */
class Nrf52PdmMic : public IPdmMic {
public:
    bool begin(int channels, long sampleRate) override {
        return PDM.begin(channels, sampleRate);
    }

    void end() override {
        PDM.end();
    }

    void setGain(int gain) override {
        PDM.setGain(gain);
    }

    void onReceive(ReceiveCallback callback) override {
        PDM.onReceive(callback);
    }

    int available() override {
        return PDM.available();
    }

    int read(int16_t* buffer, int maxBytes) override {
        return PDM.read(buffer, maxBytes);
    }
};
