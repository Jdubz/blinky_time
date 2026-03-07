#pragma once

// ============================================================================
// BeatActivationNN — TFLite Micro inference wrapper for beat activation CNN
// ============================================================================
//
// Replaces BandFlux ODF with a learned beat activation function.
// Input: 26 raw mel bands from SharedSpectralAnalysis (per frame)
// Output: beat activation (0-1) and optional downbeat activation (0-1) per frame
//
// The model is a causal 1D CNN designed to be trained on ~10K EDM tracks with
// acoustic environment augmentation. It feeds into the existing CBSS beat
// tracker — only the ODF source changes.
//
// Multi-output: if the model has 2 output channels, channel 0 = beat activation,
// channel 1 = downbeat activation. Single-channel models are backward compatible.
//
// Memory (v2, current):  ~20 KB flash, 16 KB arena + 3.3 KB context = ~19 KB RAM
// Memory (v3, planned):  ~34 KB flash, 16 KB arena + 13 KB context  = ~29 KB RAM
// Arena and context buffer are pre-sized for v3 (~18 KB over v2's needs).
// Inference: ~3-5 ms per frame (Cortex-M4F @ 64 MHz + CMSIS-NN)
//
// Enable via serial: `set nnbeat 1` (toggle A/B vs BandFlux)
// ============================================================================

#ifdef ENABLE_NN_BEAT_ACTIVATION

#include <TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "beat_model_data.h"

// Fail at compile time if NN is enabled but model data is the placeholder stub.
// A real model exported by export_tflite.py is ~20 KB; the placeholder is 4 bytes.
static_assert(beat_model_data_len > 100,
    "ENABLE_NN_BEAT_ACTIVATION requires a trained model. "
    "Run: python ml-training/scripts/export_tflite.py");

class BeatActivationNN {
public:
    static constexpr int INPUT_MEL_BANDS = 26;

    bool begin() {
        // Guard against double-initialization: static locals in this function
        // are constructed once and cannot be re-initialized. If begin() was
        // already successful, just return true.
        if (ready_) return true;

        // Skip tflite::InitializeTarget() — it reinitializes Serial at 9600
        // baud, clobbering our 115200 console. Not needed since Arduino
        // setup() already initializes Serial.

        model_ = tflite::GetModel(beat_model_data);
        if (model_ == nullptr || model_->version() != TFLITE_SCHEMA_VERSION) {
            return false;
        }

        // Register ops used by our causal CNN model.
        // Conv1D → Conv2D (TFLite internal), ZeroPadding1D → Pad,
        // BatchNorm may fuse into conv weights during export, or remain as
        // separate Mul/Add ops. ReLU is used after each conv layer.
        // If AllocateTensors fails, check tflite model ops with visualizer.
        static tflite::MicroErrorReporter micro_error_reporter;
        static tflite::MicroMutableOpResolver<10> resolver;
        resolver.AddConv2D();          // Conv1D is implemented as Conv2D internally
        resolver.AddReshape();
        resolver.AddFullyConnected();
        resolver.AddLogistic();        // Sigmoid (output layer)
        resolver.AddQuantize();
        resolver.AddDequantize();
        resolver.AddPad();             // ZeroPadding1D (causal padding)
        resolver.AddMul();             // BatchNorm (if not fused)
        resolver.AddAdd();             // BatchNorm bias (if not fused)
        resolver.AddRelu();            // ReLU activation after conv layers

        static tflite::MicroInterpreter static_interpreter(
            model_, resolver, tensorArena_, TENSOR_ARENA_SIZE,
            &micro_error_reporter);
        interpreter_ = &static_interpreter;

        if (interpreter_->AllocateTensors() != kTfLiteOk) {
            return false;
        }

        input_ = interpreter_->input(0);
        output_ = interpreter_->output(0);

        // Verify input shape matches expectations
        if (input_->dims->size < 2 ||
            input_->dims->data[input_->dims->size - 1] != INPUT_MEL_BANDS) {
            return false;
        }

        // Verify context length fits buffer
        contextLen_ = input_->dims->data[input_->dims->size - 2];
        if (contextLen_ <= 0 || contextLen_ > MAX_CONTEXT) {
            return false;
        }

        // Detect output channels: 1 = beat only, 2 = beat + downbeat
        int nDims = output_->dims->size;
        outputChannels_ = (nDims >= 2) ? output_->dims->data[nDims - 1] : 1;

        // Zero-fill context buffer
        memset(contextBuffer_, 0, contextLen_ * INPUT_MEL_BANDS * sizeof(float));
        contextWriteIdx_ = 0;
        ready_ = true;
        return true;
    }

    /**
     * Feed one frame of mel bands and get beat activation.
     * Maintains a sliding context window internally.
     * Returns beat activation in [0, 1].
     */
    float infer(const float* melBands) {
        if (!ready_) return 0.0f;

        // Shift context buffer left by one frame, append new frame
        // Context buffer is (contextLen_, INPUT_MEL_BANDS) in row-major order
        if (contextWriteIdx_ < contextLen_) {
            // Still filling up — just append
            memcpy(&contextBuffer_[contextWriteIdx_ * INPUT_MEL_BANDS],
                   melBands, INPUT_MEL_BANDS * sizeof(float));
            contextWriteIdx_++;
        } else {
            // Shift left by one frame using memmove (handles overlap)
            memmove(contextBuffer_, contextBuffer_ + INPUT_MEL_BANDS,
                    (contextLen_ - 1) * INPUT_MEL_BANDS * sizeof(float));
            // Append new frame at end
            memcpy(&contextBuffer_[(contextLen_ - 1) * INPUT_MEL_BANDS],
                   melBands, INPUT_MEL_BANDS * sizeof(float));
        }

        // Don't run inference until we have a full context window
        if (contextWriteIdx_ < contextLen_) {
            return 0.0f;
        }

        // Copy context to input tensor (handles quantization if INT8)
        if (input_->type == kTfLiteInt8) {
            float scale = input_->params.scale;
            int32_t zero_point = input_->params.zero_point;
            int8_t* input_data = input_->data.int8;
            for (int i = 0; i < contextLen_ * INPUT_MEL_BANDS; i++) {
                int32_t quantized = static_cast<int32_t>(contextBuffer_[i] / scale + zero_point);
                if (quantized < -128) quantized = -128;
                if (quantized > 127) quantized = 127;
                input_data[i] = static_cast<int8_t>(quantized);
            }
        } else {
            memcpy(input_->data.f, contextBuffer_,
                   contextLen_ * INPUT_MEL_BANDS * sizeof(float));
        }

        // Run inference
        if (interpreter_->Invoke() != kTfLiteOk) {
            return 0.0f;
        }

        // Extract outputs from last frame
        int lastFrame = contextLen_ - 1;
        float beat = extractOutput(lastFrame, 0);
        lastDownbeat_ = (outputChannels_ >= 2) ? extractOutput(lastFrame, 1) : 0.0f;

        return beat;
    }

    bool isReady() const { return ready_; }

    /** Get last downbeat activation (0-1). Only valid after infer(). */
    float getLastDownbeat() const { return lastDownbeat_; }

    /** Whether model has a downbeat output head. */
    bool hasDownbeatOutput() const { return outputChannels_ >= 2; }

private:
    float extractOutput(int frame, int channel) {
        int idx = frame * outputChannels_ + channel;
        if (idx >= contextLen_ * outputChannels_) return 0.0f;

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

    // Tensor arena — pre-allocated, no dynamic memory
    // 16 KB: sized for v3 wider model (v2 needs ~8 KB, but pre-provisioned
    // to avoid a firmware update when v3 ships)
    static constexpr int TENSOR_ARENA_SIZE = 16384;
    alignas(16) uint8_t tensorArena_[TENSOR_ARENA_SIZE];

    // Context buffer for sliding window — pre-sized for v3 wider model.
    // v2 uses 32 frames; v3 needs up to 128. Runtime contextLen_ is set from
    // the model's actual input shape, so only the needed portion is used.
    static constexpr int MAX_CONTEXT = 128;
    float contextBuffer_[MAX_CONTEXT * INPUT_MEL_BANDS];
    int contextLen_ = 0;      // Actual context length from model input shape
    int contextWriteIdx_ = 0; // How many frames we've written so far

    const tflite::Model* model_ = nullptr;
    tflite::MicroInterpreter* interpreter_ = nullptr;
    TfLiteTensor* input_ = nullptr;
    TfLiteTensor* output_ = nullptr;
    int outputChannels_ = 1;  // 1 = beat only, 2 = beat + downbeat
    float lastDownbeat_ = 0.0f;
    bool ready_ = false;
};

#else

// Stub when NN beat activation is not compiled in
class BeatActivationNN {
public:
    static constexpr int INPUT_MEL_BANDS = 26;
    bool begin() { return false; }
    float infer(const float*) { return 0.0f; }
    bool isReady() const { return false; }
    float getLastDownbeat() const { return 0.0f; }
    bool hasDownbeatOutput() const { return false; }
};

#endif // ENABLE_NN_BEAT_ACTIVATION
