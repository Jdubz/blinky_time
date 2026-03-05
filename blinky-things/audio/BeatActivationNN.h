#pragma once

// ============================================================================
// BeatActivationNN — TFLite Micro inference wrapper for beat activation CNN
// ============================================================================
//
// Replaces BandFlux ODF with a learned beat activation function.
// Input: 26 mel bands from SharedSpectralAnalysis (per frame)
// Output: beat activation (0-1) per frame
//
// The model is a causal 1D CNN trained on ~10K EDM tracks with acoustic
// environment augmentation. It feeds into the existing CBSS beat tracker —
// only the ODF source changes.
//
// Memory: ~20 KB flash (model weights) + ~8 KB RAM (tensor arena)
// Inference: ~3-5 ms per frame (Cortex-M4F @ 64 MHz + CMSIS-NN)
//
// Enable via serial: `set nnbeat 1` (toggle A/B vs BandFlux)
// ============================================================================

#ifdef ENABLE_NN_BEAT_ACTIVATION

#include <TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "beat_model_data.h"

class BeatActivationNN {
public:
    static constexpr int INPUT_MEL_BANDS = 26;

    bool begin() {
        tflite::InitializeTarget();

        model_ = tflite::GetModel(beat_model_data);
        if (model_->version() != TFLITE_SCHEMA_VERSION) {
            return false;
        }

        // Register ops used by our causal CNN model.
        // Conv1D → Conv2D (TFLite internal), ZeroPadding1D → Pad,
        // BatchNorm fuses into conv weights during export (usually).
        // If AllocateTensors fails, check tflite model ops with visualizer.
        static tflite::MicroErrorReporter micro_error_reporter;
        static tflite::MicroMutableOpResolver<8> resolver;
        resolver.AddConv2D();          // Conv1D is implemented as Conv2D internally
        resolver.AddReshape();
        resolver.AddFullyConnected();
        resolver.AddLogistic();        // Sigmoid
        resolver.AddQuantize();
        resolver.AddDequantize();
        resolver.AddPad();             // ZeroPadding1D (causal padding)
        resolver.AddMul();             // BatchNorm (if not fused during conversion)

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
        if (contextLen_ > MAX_CONTEXT) {
            return false;  // Model needs more context than buffer allows
        }

        // Fill context buffer with zeros
        for (int i = 0; i < contextLen_ * INPUT_MEL_BANDS; i++) {
            contextBuffer_[i] = 0;
        }
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
            for (int i = 0; i < INPUT_MEL_BANDS; i++) {
                contextBuffer_[contextWriteIdx_ * INPUT_MEL_BANDS + i] = melBands[i];
            }
            contextWriteIdx_++;
        } else {
            // Shift left by one frame
            for (int i = 0; i < (contextLen_ - 1) * INPUT_MEL_BANDS; i++) {
                contextBuffer_[i] = contextBuffer_[i + INPUT_MEL_BANDS];
            }
            // Append new frame at end
            for (int i = 0; i < INPUT_MEL_BANDS; i++) {
                contextBuffer_[(contextLen_ - 1) * INPUT_MEL_BANDS + i] = melBands[i];
            }
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
            float* input_data = input_->data.f;
            for (int i = 0; i < contextLen_ * INPUT_MEL_BANDS; i++) {
                input_data[i] = contextBuffer_[i];
            }
        }

        // Run inference
        if (interpreter_->Invoke() != kTfLiteOk) {
            return 0.0f;
        }

        // Extract output — take the last frame's activation
        float activation;
        if (output_->type == kTfLiteInt8) {
            float scale = output_->params.scale;
            int32_t zero_point = output_->params.zero_point;
            // Output shape is (1, contextLen_, 1) — take last frame
            int last_idx = contextLen_ - 1;
            activation = (output_->data.int8[last_idx] - zero_point) * scale;
        } else {
            int last_idx = contextLen_ - 1;
            activation = output_->data.f[last_idx];
        }

        // Clamp to [0, 1]
        if (activation < 0.0f) activation = 0.0f;
        if (activation > 1.0f) activation = 1.0f;

        return activation;
    }

    bool isReady() const { return ready_; }

private:
    // Tensor arena — pre-allocated, no dynamic memory
    // 8 KB should be sufficient for our 9K-param model
    static constexpr int TENSOR_ARENA_SIZE = 8192;
    alignas(16) uint8_t tensorArena_[TENSOR_ARENA_SIZE];

    // Context buffer for sliding window
    // Model is exported with --inference-frames (default 32 = 512ms at 62.5 Hz).
    // Receptive field is 15 frames; 32 provides margin.
    // Set MAX_CONTEXT >= the exported model's input time dimension.
    static constexpr int MAX_CONTEXT = 32;
    float contextBuffer_[MAX_CONTEXT * INPUT_MEL_BANDS];
    int contextLen_ = 0;      // Actual context length from model input shape
    int contextWriteIdx_ = 0; // How many frames we've written so far

    const tflite::Model* model_ = nullptr;
    tflite::MicroInterpreter* interpreter_ = nullptr;
    TfLiteTensor* input_ = nullptr;
    TfLiteTensor* output_ = nullptr;
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
};

#endif // ENABLE_NN_BEAT_ACTIVATION
