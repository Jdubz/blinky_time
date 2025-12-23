#pragma once
#include <stdint.h>

/**
 * IPdmMic - Abstract interface for PDM microphone
 *
 * Used by AdaptiveMic for audio input.
 * Enables unit testing with mocks.
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
};
