#include "hal/hardware/Nrf52PdmMic.h"
#include <PDM.h>

bool Nrf52PdmMic::begin(int channels, long sampleRate) {
    return PDM.begin(channels, sampleRate);
}

void Nrf52PdmMic::end() {
    PDM.end();
}

void Nrf52PdmMic::setGain(int gain) {
    PDM.setGain(gain);
}

void Nrf52PdmMic::onReceive(ReceiveCallback callback) {
    PDM.onReceive(callback);
}

int Nrf52PdmMic::available() {
    return PDM.available();
}

int Nrf52PdmMic::read(int16_t* buffer, int maxBytes) {
    return PDM.read(buffer, maxBytes);
}
