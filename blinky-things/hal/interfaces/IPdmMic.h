#pragma once
#include <stdint.h>

/**
 * IPdmMic - Abstract interface for PDM microphone
 *
 * Used by AdaptiveMic for audio input.
 * Enables unit testing with mocks.
 *
 * Capability queries allow AdaptiveMic to adapt AGC behaviour at runtime
 * without compile-time platform branches.  Implementations override these
 * to advertise what their hardware can actually do.
 */
class IPdmMic {
public:
    virtual ~IPdmMic() = default;

    // Callback type for ISR (data ready notification)
    using ReceiveCallback = void(*)();

    // Lifecycle
    virtual bool begin(int channels, long sampleRate) = 0;
    virtual void end() = 0;

    // Configuration
    virtual void setGain(int gain) = 0;
    virtual void onReceive(ReceiveCallback callback) = 0;

    // Data access
    virtual int available() = 0;
    virtual int read(int16_t* buffer, int maxBytes) = 0;

    // Polling hook — called once per frame from AdaptiveMic::update().
    // nRF52: no-op (hardware PDM interrupt fires the callback automatically).
    // ESP32: reads I2S DMA buffer into staging and fires the stored callback.
    virtual void poll() {}

    // ---- Capability queries ----
    // True if setGain() adjusts real hardware registers (pre-decimation, improves SNR).
    // False if gain is applied in software post-decimation (does not improve SNR).
    virtual bool hasHardwareGain() const { return false; }

    // Effective gain range advertised to AdaptiveMic's AGC.
    // Hardware platforms: physical register limits.
    // Software platforms: practical software range (0–80 dB, 1 dB precision).
    virtual int   getGainMinDb()   const { return 0; }
    virtual int   getGainMaxDb()   const { return 80; }
    virtual float getGainStepDb()  const { return 1.0f; }
};
