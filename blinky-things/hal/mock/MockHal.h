#pragma once
#include "../interfaces/IGpio.h"
#include "../interfaces/IAdc.h"
#include "../interfaces/ISystemTime.h"
#include "../interfaces/IPdmMic.h"

/**
 * MockGpio - Test mock for GPIO operations
 *
 * Simulates GPIO pins with configurable input values.
 * Uses simple arrays for Arduino compatibility.
 */
class MockGpio : public IGpio {
public:
    static constexpr int MAX_PINS = 64;

    MockGpio() {
        reset();
    }

    void pinMode(int pin, uint8_t mode) override {
        if (pin >= 0 && pin < MAX_PINS) {
            pinModes_[pin] = mode;
        }
    }

    void digitalWrite(int pin, uint8_t value) override {
        if (pin >= 0 && pin < MAX_PINS) {
            outputValues_[pin] = value;
        }
    }

    int digitalRead(int pin) const override {
        if (pin >= 0 && pin < MAX_PINS) {
            return inputValues_[pin];
        }
        return LOW_LEVEL;
    }

    // Test helpers
    void setDigitalInput(int pin, int value) {
        if (pin >= 0 && pin < MAX_PINS) {
            inputValues_[pin] = value;
        }
    }

    uint8_t getPinMode(int pin) const {
        return (pin >= 0 && pin < MAX_PINS) ? pinModes_[pin] : 0;
    }

    int getDigitalOutput(int pin) const {
        return (pin >= 0 && pin < MAX_PINS) ? outputValues_[pin] : 0;
    }

    void reset() {
        for (int i = 0; i < MAX_PINS; i++) {
            pinModes_[i] = INPUT_MODE;
            outputValues_[i] = LOW_LEVEL;
            inputValues_[i] = LOW_LEVEL;
        }
    }

private:
    uint8_t pinModes_[MAX_PINS];
    int outputValues_[MAX_PINS];
    int inputValues_[MAX_PINS];
};

/**
 * MockAdc - Test mock for ADC operations
 *
 * Returns configurable values for analog reads.
 */
class MockAdc : public IAdc {
public:
    static constexpr int MAX_PINS = 16;

    MockAdc() : resolution_(10), reference_(REF_DEFAULT) {
        reset();
    }

    void setResolution(uint8_t bits) override {
        resolution_ = bits;
    }

    void setReference(uint8_t reference) override {
        reference_ = reference;
    }

    uint16_t analogRead(int pin) override {
        if (pin >= 0 && pin < MAX_PINS) {
            return inputValues_[pin];
        }
        return 0;
    }

    // Test helpers
    void setAnalogInput(int pin, uint16_t value) {
        if (pin >= 0 && pin < MAX_PINS) {
            inputValues_[pin] = value;
        }
    }

    uint8_t getResolution() const { return resolution_; }
    uint8_t getReference() const { return reference_; }

    void reset() {
        resolution_ = 10;
        reference_ = REF_DEFAULT;
        for (int i = 0; i < MAX_PINS; i++) {
            inputValues_[i] = 0;
        }
    }

private:
    uint16_t inputValues_[MAX_PINS];
    uint8_t resolution_;
    uint8_t reference_;
};

/**
 * MockSystemTime - Test mock for system timing
 *
 * Allows tests to control time progression.
 */
class MockSystemTime : public ISystemTime {
public:
    MockSystemTime() : currentMillis_(0), currentMicros_(0), interruptsDisabled_(false) {}

    uint32_t millis() const override { return currentMillis_; }
    uint32_t micros() const override { return currentMicros_; }

    void delay(uint32_t ms) override {
        currentMillis_ += ms;
        currentMicros_ += ms * 1000;
    }

    void delayMicroseconds(uint32_t us) override {
        currentMicros_ += us;
        currentMillis_ += us / 1000;
    }

    void noInterrupts() override { interruptsDisabled_ = true; }
    void interrupts() override { interruptsDisabled_ = false; }

    // Test helpers
    void advanceMillis(uint32_t ms) {
        currentMillis_ += ms;
        currentMicros_ += ms * 1000;
    }

    void advanceMicros(uint32_t us) {
        currentMicros_ += us;
        currentMillis_ += us / 1000;
    }

    void setMillis(uint32_t ms) {
        currentMillis_ = ms;
        currentMicros_ = ms * 1000;
    }

    bool areInterruptsDisabled() const { return interruptsDisabled_; }

    void reset() {
        currentMillis_ = 0;
        currentMicros_ = 0;
        interruptsDisabled_ = false;
    }

private:
    uint32_t currentMillis_;
    uint32_t currentMicros_;
    bool interruptsDisabled_;
};

/**
 * MockPdmMic - Test mock for PDM microphone
 *
 * Simulates audio data for testing.
 */
class MockPdmMic : public IPdmMic {
public:
    static constexpr int MAX_BUFFER_SIZE = 512;

    MockPdmMic() : callback_(nullptr), gain_(0), running_(false),
                   bufferSize_(0), bufferRead_(0) {}

    bool begin(int channels, long sampleRate) override {
        channels_ = channels;
        sampleRate_ = sampleRate;
        running_ = true;
        return true;
    }

    void end() override {
        running_ = false;
    }

    void setGain(int gain) override {
        gain_ = gain;
    }

    void onReceive(ReceiveCallback callback) override {
        callback_ = callback;
    }

    int available() override {
        return (bufferSize_ - bufferRead_) * sizeof(int16_t);
    }

    int read(int16_t* buffer, int maxBytes) override {
        int samplesToRead = maxBytes / sizeof(int16_t);
        int samplesAvailable = bufferSize_ - bufferRead_;
        int actualSamples = (samplesToRead < samplesAvailable) ? samplesToRead : samplesAvailable;

        for (int i = 0; i < actualSamples; i++) {
            buffer[i] = audioBuffer_[bufferRead_ + i];
        }
        bufferRead_ += actualSamples;

        return actualSamples * sizeof(int16_t);
    }

    // Test helpers
    void simulateAudioData(const int16_t* samples, int count) {
        int toCopy = (count < MAX_BUFFER_SIZE) ? count : MAX_BUFFER_SIZE;
        for (int i = 0; i < toCopy; i++) {
            audioBuffer_[i] = samples[i];
        }
        bufferSize_ = toCopy;
        bufferRead_ = 0;
    }

    void triggerCallback() {
        if (callback_) {
            callback_();
        }
    }

    int getGain() const { return gain_; }
    bool isRunning() const { return running_; }
    int getChannels() const { return channels_; }
    long getSampleRate() const { return sampleRate_; }

    void reset() {
        callback_ = nullptr;
        gain_ = 0;
        running_ = false;
        bufferSize_ = 0;
        bufferRead_ = 0;
        channels_ = 0;
        sampleRate_ = 0;
    }

private:
    ReceiveCallback callback_;
    int16_t audioBuffer_[MAX_BUFFER_SIZE];
    int gain_;
    bool running_;
    int bufferSize_;
    int bufferRead_;
    int channels_;
    long sampleRate_;
};
