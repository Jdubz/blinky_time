#pragma once

// ============================================================================
// FrameBeatNN — TFLite Micro inference for frame-level beat/downbeat
// ============================================================================
//
// Supports two model architectures via the same sliding window interface:
//   FC:     N×26 flat → Dense layers → 2 outputs      (~0.1ms, ~5 KB arena)
//   Conv1D: N×26 3D → causal Conv1D → 2 per-timestep  (~5-8ms, ~16-20 KB arena)
//
// Runs every spectral frame (~62.5 Hz). Conv1D uses 5-8ms of the 16ms frame.
// Replaces BandFlux ODF for CBSS beat tracking; BandFlux still runs
// for transient detection (sparks/effects).
//
// Model type auto-detected from TFLite input/output shapes.
// Conv1D output is (1, W, 2); firmware extracts last timestep.
// Memory: ~16 KB tensor arena (covers both architectures) + sliding window buffer
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

        // Only initialize if we have a real model (not the 4-byte placeholder stub).
        // Runtime check allows scaffold builds; when a trained model is committed,
        // replace with static_assert(frame_beat_model_data_len > 100, ...) to catch
        // accidental stub-model NN=1 builds at compile time.
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

        // Op resolver supports both FC and Conv1D models:
        //   FC:     FullyConnected, ReLU, Logistic, Quantize, Dequantize
        //   Conv1D: Conv2D (mapped from Conv1D), Pad, Reshape, Logistic, Quantize, Dequantize
        // ReLU is fused into Conv2D activation for Conv1D models, but we register it
        // separately for FC compatibility.
        static tflite::MicroErrorReporter error_reporter;
        static tflite::MicroMutableOpResolver<8> resolver;
        // Guard: C++ guarantees static locals init once, but Add*() is not
        // idempotent — duplicate registration wastes resolver slots. The guard
        // protects against a hypothetical retry path even though begin() is
        // documented as not safe to retry after failure.
        static bool resolverInited = false;
        if (!resolverInited) {
            resolver.AddFullyConnected();  // FC model layers
            resolver.AddConv2D();          // Conv1D mapped to Conv2D
            resolver.AddPad();             // Causal padding (ZeroPadding1D)
            resolver.AddReshape();         // 1D↔2D tensor conversion
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

        // Detect output channels and compute offset to last timestep.
        // FC output: (1, 2) → totalOutputs=2, outputChannels_=2, outputOffset_=0
        // Conv1D output: (1, W, 2) → totalOutputs=W*2, outputChannels_=2, outputOffset_=(W-1)*2
        int totalOutputs = 1;
        for (int i = 0; i < output_->dims->size; i++) {
            totalOutputs *= output_->dims->data[i];
        }
        // Last dimension is channel count
        outputChannels_ = output_->dims->data[output_->dims->size - 1];
        if (outputChannels_ < 1 || outputChannels_ > 4 || totalOutputs < outputChannels_) {
            initError_ = 7;  // Unexpected output shape
            return false;
        }
        // Offset to last timestep (0 for FC, (W-1)*channels for Conv1D)
        outputOffset_ = totalOutputs - outputChannels_;

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

        // Periodic timing output when profiling enabled via `set nnprofile 1`
        if (profileEnabled_ && (inferCount_ % 100 == 0)) {
            Serial.print(F("[NNPROF] cnt="));
            Serial.print(inferCount_);
            Serial.print(F(" infer="));
            Serial.print(lastInferUs_);
            Serial.print(F("us arena="));
            Serial.print(arenaUsed_);
            Serial.print(F("/"));
            Serial.print(TENSOR_ARENA_SIZE);
            Serial.println();
        }

        // Capture raw INT8 output for diagnostics (before dequantization)
        if (output_->type == kTfLiteInt8) {
            lastRawOut0_ = output_->data.int8[outputOffset_];
            lastRawOut1_ = (outputChannels_ >= 2) ? output_->data.int8[outputOffset_ + 1] : 0;
        }

        // Extract beat activation (output channel 0)
        float beat = extractOutput(0);
        lastDownbeat_ = (outputChannels_ >= 2) ? extractOutput(1) : 0.0f;
        lastBeat_ = beat;

        return beat;
    }

    bool isReady() const { return ready_; }

    /** Get last downbeat activation (0-1). Only valid after infer(). */
    float getLastDownbeat() const { return lastDownbeat_; }

    /** Whether model has a downbeat output head. */
    bool hasDownbeatOutput() const { return outputChannels_ >= 2; }

    /** Enable/disable periodic timing output to Serial. */
    void setProfileEnabled(bool enabled) { profileEnabled_ = enabled; }
    bool isProfileEnabled() const { return profileEnabled_; }
    unsigned long getLastInferUs() const { return lastInferUs_; }

    /** Get count of failed Invoke() calls. */
    uint32_t getInvokeErrors() const { return invokeErrors_; }
    uint32_t getInferCount() const { return inferCount_; }

    /** Get last beat activation (for diagnostics). */
    float getLastBeat() const { return lastBeat_; }

    /** Get raw INT8 output values for debugging quantization issues. */
    int8_t getLastRawOut0() const { return lastRawOut0_; }
    int8_t getLastRawOut1() const { return lastRawOut1_; }

    /** Get input/output quantization parameters for debugging. */
    float getInputScale() const { return input_ ? input_->params.scale : 0.0f; }
    int32_t getInputZeroPoint() const { return input_ ? input_->params.zero_point : 0; }
    float getOutputScale() const { return output_ ? output_->params.scale : 0.0f; }
    int32_t getOutputZeroPoint() const { return output_ ? output_->params.zero_point : 0; }

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
            // Quantization debug: input scale/zp, output scale/zp, last raw outputs
            Serial.print(F("\n[FrameBeatNN] iscale="));
            Serial.print(input_->params.scale, 8);
            Serial.print(F(" izp="));
            Serial.print(input_->params.zero_point);
            Serial.print(F(" oscale="));
            Serial.print(output_->params.scale, 8);
            Serial.print(F(" ozp="));
            Serial.print(output_->params.zero_point);
            Serial.print(F("\n[FrameBeatNN] raw_out=["));
            Serial.print(lastRawOut0_);
            Serial.print(F(","));
            Serial.print(lastRawOut1_);
            Serial.print(F("] beat="));
            Serial.print(lastBeat_, 4);
            Serial.print(F(" db="));
            Serial.print(lastDownbeat_, 4);
            // Print last frame's input: first and last mel band INT8 values
            if (input_->type == kTfLiteInt8 && windowFilled_ >= windowFrames_) {
                int lastFrameStart = (windowFrames_ - 1) * INPUT_MEL_BANDS;
                Serial.print(F("\n[FrameBeatNN] last_input_float=["));
                Serial.print(windowBuffer_[lastFrameStart], 4);
                Serial.print(F(","));
                Serial.print(windowBuffer_[lastFrameStart + 1], 4);
                Serial.print(F(",...,"));
                Serial.print(windowBuffer_[lastFrameStart + INPUT_MEL_BANDS - 1], 4);
                Serial.print(F("] int8=["));
                Serial.print(input_->data.int8[lastFrameStart]);
                Serial.print(F(","));
                Serial.print(input_->data.int8[lastFrameStart + 1]);
                Serial.print(F(",...,"));
                Serial.print(input_->data.int8[lastFrameStart + INPUT_MEL_BANDS - 1]);
                Serial.print(F("]"));
            }
        } else {
            Serial.print(F(" error="));
            Serial.print(initError_);
        }
        Serial.println();
    }

private:
    float extractOutput(int channel) {
        if (channel < 0 || channel >= outputChannels_) return 0.0f;
        int idx = outputOffset_ + channel;
        float value;
        if (output_->type == kTfLiteInt8) {
            float scale = output_->params.scale;
            int32_t zero_point = output_->params.zero_point;
            value = (output_->data.int8[idx] - zero_point) * scale;
        } else {
            value = output_->data.f[idx];
        }
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        return value;
    }

    // Tensor arena — must fit both FC and Conv1D models:
    //   FC:     ~2-4 KB (matrix multiplies on flat input)
    //   Conv1D: ~12-20 KB (intermediate activations: 32 frames × up to 48 channels)
    // FC model uses ~2 KB arena. Conv1D estimated ~12-16 KB.
    // 16 KB covers both with headroom. Check `show nn` for actual usage.
    static constexpr int TENSOR_ARENA_SIZE = 16384;  // 16 KB
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
    int outputOffset_ = 0;     // Byte offset to last timestep (0 for FC, (W-1)*ch for Conv1D)
    float lastDownbeat_ = 0.0f;
    float lastBeat_ = 0.0f;
    int8_t lastRawOut0_ = 0;  // Raw INT8 output for beat (before dequantization)
    int8_t lastRawOut1_ = 0;  // Raw INT8 output for downbeat
    bool ready_ = false;
    bool profileEnabled_ = false;
    uint32_t inferCount_ = 0;
    int initError_ = 0;  // 0=not attempted, 1=null model, 2=schema, 3=alloc, 4=placeholder, 5=dim, 6=window too large, 7=bad output shape
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
    unsigned long getLastInferUs() const { return 0; }
    uint32_t getInvokeErrors() const { return 0; }
    uint32_t getInferCount() const { return 0; }
    void printDiagnostics() const { Serial.println(F("[FrameBeatNN] not compiled")); }
};

#endif // ENABLE_NN_BEAT_ACTIVATION
