#pragma once

// ============================================================================
// FrameBeatNN — TFLite Micro inference for frame-level FC beat/downbeat
// ============================================================================
//
// Frame-level FC model: sliding window of N raw mel frames × 26 bands
// → FC hidden layers → beat_activation + downbeat_activation.
//
// Runs every Kth frame (~15.6 Hz at K=4, 62.5 Hz mic rate).
// Replaces BandFlux ODF for CBSS beat tracking; BandFlux still runs
// for transient detection (sparks/effects).
//
// Architecture: FC-only (N×26 → hidden → 2), ~5-15K params
// Inference: ~60-200µs on Cortex-M4F @ 64 MHz (negligible)
// Memory: ~4-8 KB tensor arena + sliding window buffer
//
// Input: raw mel bands from SharedSpectralAnalysis::getRawMelBands()
// These depend only on 8 fundamental constants (sample rate, FFT size,
// hop, mel bands, mel range, mel scale, log compression, window).
// Changes to compressor, whitening, BandFlux, etc. do NOT require retraining.
//
// Enable via: compile with ENABLE_NN_BEAT_ACTIVATION
// ============================================================================

#ifdef ENABLE_NN_BEAT_ACTIVATION

#include <TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "frame_beat_model_data.h"

class FrameBeatNN {
public:
    static constexpr int INPUT_MEL_BANDS = 26;

    /**
     * Initialize TFLite interpreter and allocate tensors.
     * Must be called once at startup. Not safe to retry after failure —
     * static locals are constructed exactly once.
     */
    bool begin() {
        if (ready_) return true;

        // Only initialize if we have a real model (not the 4-byte placeholder stub)
        if (frame_beat_model_data_len < 100) {
            initError_ = 4;  // Placeholder model
            return false;
        }

        model_ = tflite::GetModel(frame_beat_model_data);
        if (model_ == nullptr) {
            initError_ = 1;
            return false;
        }
        if (model_->version() != TFLITE_SCHEMA_VERSION) {
            initError_ = 2;
            return false;
        }

        // FC-only model needs: FullyConnected + Logistic (sigmoid) + ReLU
        // Quantize/Dequantize for INT8 I/O
        static tflite::MicroErrorReporter error_reporter;
        static tflite::MicroMutableOpResolver<5> resolver;
        static bool resolverInited = false;
        if (!resolverInited) {
            resolver.AddFullyConnected();
            resolver.AddLogistic();        // Sigmoid output
            resolver.AddRelu();            // Hidden layer activations
            resolver.AddQuantize();
            resolver.AddDequantize();
            resolverInited = true;
        }

        static tflite::MicroInterpreter static_interpreter(
            model_, resolver, tensorArena_, TENSOR_ARENA_SIZE,
            &error_reporter);
        interpreter_ = &static_interpreter;

        TfLiteStatus allocStatus = interpreter_->AllocateTensors();
        if (allocStatus != kTfLiteOk) {
            initError_ = 3;
            return false;
        }
        arenaUsed_ = interpreter_->arena_used_bytes();

        input_ = interpreter_->input(0);
        output_ = interpreter_->output(0);

        // Detect input window size from model shape
        int inputSize = 1;
        for (int i = 0; i < input_->dims->size; i++) {
            inputSize *= input_->dims->data[i];
        }
        // inputSize should be windowFrames * INPUT_MEL_BANDS
        if (inputSize < INPUT_MEL_BANDS || inputSize % INPUT_MEL_BANDS != 0) {
            initError_ = 5;  // Input dimension mismatch
            return false;
        }
        windowFrames_ = inputSize / INPUT_MEL_BANDS;
        if (windowFrames_ > MAX_WINDOW_FRAMES) {
            initError_ = 6;  // Window too large
            return false;
        }

        // Detect output channels: 1 = beat only, 2 = beat + downbeat
        int outSize = 1;
        for (int i = 0; i < output_->dims->size; i++) {
            outSize *= output_->dims->data[i];
        }
        outputChannels_ = outSize;

        // Zero-fill sliding window buffer
        memset(windowBuffer_, 0, sizeof(windowBuffer_));
        windowFilled_ = 0;

        ready_ = true;
        return true;
    }

    /**
     * Feed one frame of mel bands and get beat activation.
     * Maintains a sliding window internally.
     * Returns beat activation in [0, 1].
     */
    float infer(const float* melBands) {
        if (!ready_) return 0.0f;

        // Shift window left by one frame, append new frame
        if (windowFilled_ < windowFrames_) {
            memcpy(&windowBuffer_[windowFilled_ * INPUT_MEL_BANDS],
                   melBands, INPUT_MEL_BANDS * sizeof(float));
            windowFilled_++;
        } else {
            memmove(windowBuffer_, windowBuffer_ + INPUT_MEL_BANDS,
                    (windowFrames_ - 1) * INPUT_MEL_BANDS * sizeof(float));
            memcpy(&windowBuffer_[(windowFrames_ - 1) * INPUT_MEL_BANDS],
                   melBands, INPUT_MEL_BANDS * sizeof(float));
        }

        // Don't run inference until we have a full window
        if (windowFilled_ < windowFrames_) {
            return 0.0f;
        }

        // Quantize and copy to input tensor
        int totalInputs = windowFrames_ * INPUT_MEL_BANDS;
        if (input_->type == kTfLiteInt8) {
            float scale = input_->params.scale;
            if (scale <= 0.0f) { invokeErrors_++; return 0.0f; }
            int32_t zero_point = input_->params.zero_point;
            int8_t* input_data = input_->data.int8;
            for (int i = 0; i < totalInputs; i++) {
                int32_t q = static_cast<int32_t>(windowBuffer_[i] / scale + zero_point);
                if (q < -128) q = -128;
                if (q > 127) q = 127;
                input_data[i] = static_cast<int8_t>(q);
            }
        } else {
            memcpy(input_->data.f, windowBuffer_, totalInputs * sizeof(float));
        }

        // Run inference
        unsigned long t0 = micros();
        if (interpreter_->Invoke() != kTfLiteOk) {
            invokeErrors_++;
            return 0.0f;
        }
        lastInferUs_ = micros() - t0;
        inferCount_++;

        // Extract beat activation (output channel 0)
        float beat = extractOutput(0);
        lastDownbeat_ = (outputChannels_ >= 2) ? extractOutput(1) : 0.0f;

        return beat;
    }

    bool isReady() const { return ready_; }

    /** Get last downbeat activation (0-1). Only valid after infer(). */
    float getLastDownbeat() const { return lastDownbeat_; }

    /** Whether model has a downbeat output head. */
    bool hasDownbeatOutput() const { return outputChannels_ >= 2; }

    /** Enable/disable per-operator profiling output to Serial. */
    void setProfileEnabled(bool enabled) { profileEnabled_ = enabled; }
    bool isProfileEnabled() const { return profileEnabled_; }

    /** Get count of failed Invoke() calls. */
    uint32_t getInvokeErrors() const { return invokeErrors_; }
    uint32_t getInferCount() const { return inferCount_; }

    /** Print diagnostic info to Serial. */
    void printDiagnostics() const {
        Serial.print(F("[FrameBeatNN] ready="));
        Serial.print(ready_ ? F("yes") : F("no"));
        if (ready_) {
            Serial.print(F(" arena="));
            Serial.print(arenaUsed_);
            Serial.print(F("/"));
            Serial.print(TENSOR_ARENA_SIZE);
            Serial.print(F(" window="));
            Serial.print(windowFrames_);
            Serial.print(F(" outputs="));
            Serial.print(outputChannels_);
            Serial.print(F(" infer="));
            Serial.print(lastInferUs_);
            Serial.print(F("us cnt="));
            Serial.print(inferCount_);
            if (invokeErrors_ > 0) {
                Serial.print(F(" ERRORS="));
                Serial.print(invokeErrors_);
            }
        } else {
            Serial.print(F(" error="));
            Serial.print(initError_);
        }
        Serial.println();
    }

private:
    float extractOutput(int channel) {
        float value;
        if (output_->type == kTfLiteInt8) {
            float scale = output_->params.scale;
            int32_t zero_point = output_->params.zero_point;
            value = (output_->data.int8[channel] - zero_point) * scale;
        } else {
            value = output_->data.f[channel];
        }
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        return value;
    }

    // Tensor arena — FC-only model needs ~4-8 KB
    static constexpr int TENSOR_ARENA_SIZE = 8192;  // 8 KB
    alignas(16) uint8_t tensorArena_[TENSOR_ARENA_SIZE];

    // Sliding window buffer for mel frames
    // Max window: 64 frames (~1 second at 62.5 Hz). Actual size set from model.
    static constexpr int MAX_WINDOW_FRAMES = 64;
    float windowBuffer_[MAX_WINDOW_FRAMES * INPUT_MEL_BANDS];
    int windowFrames_ = 0;     // Actual window size from model input shape
    int windowFilled_ = 0;     // How many frames we've written so far

    const tflite::Model* model_ = nullptr;
    tflite::MicroInterpreter* interpreter_ = nullptr;
    TfLiteTensor* input_ = nullptr;
    TfLiteTensor* output_ = nullptr;
    int outputChannels_ = 1;   // 1 = beat only, 2 = beat + downbeat
    float lastDownbeat_ = 0.0f;
    bool ready_ = false;
    bool profileEnabled_ = false;
    uint32_t inferCount_ = 0;
    int initError_ = 0;  // 0=not attempted, 1=null model, 2=schema, 3=alloc, 4=placeholder, 5=dim, 6=window too large
    size_t arenaUsed_ = 0;
    unsigned long lastInferUs_ = 0;
    uint32_t invokeErrors_ = 0;
};

#else

// Stub when NN is not compiled in
class FrameBeatNN {
public:
    static constexpr int INPUT_MEL_BANDS = 26;
    bool begin() { return false; }
    float infer(const float*) { return 0.0f; }
    bool isReady() const { return false; }
    float getLastDownbeat() const { return 0.0f; }
    bool hasDownbeatOutput() const { return false; }
    void setProfileEnabled(bool) {}
    bool isProfileEnabled() const { return false; }
    uint32_t getInvokeErrors() const { return 0; }
    uint32_t getInferCount() const { return 0; }
    void printDiagnostics() const { Serial.println(F("[FrameBeatNN] not compiled")); }
};

#endif // ENABLE_NN_BEAT_ACTIVATION
