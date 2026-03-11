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
// Memory (5L v4/v6):  ~33 KB flash, ~14 KB arena + 13 KB context (128 frames) = ~27 KB RAM
// Memory (7L v7):     ~46 KB flash, ~28 KB arena + 27 KB context (256 frames) = ~55 KB RAM
// Memory (DS-TCN v9): ~26 KB flash, ~34 KB arena + 13 KB context (128 frames) = ~47 KB RAM
// Arena sized to 96 KB (generous headroom). Context pre-sized for 256 frames.
// Runtime contextLen_ adapts to actual model.
// Inference: ~79 ms per frame (5L BN-fused), target ~25-30 ms (v9 DS-TCN) (Cortex-M4F @ 64 MHz)
// Profiling: MicroProfiler prints per-op timing every 50 frames when nnprofile=1.
//
// Enable via serial: `set nnbeat 1` (toggle A/B vs BandFlux)
// ============================================================================

#ifdef ENABLE_NN_BEAT_ACTIVATION

#include <TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_profiler.h"
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
        if (model_ == nullptr) {
            initError_ = 1;
            return false;
        }
        if (model_->version() != TFLITE_SCHEMA_VERSION) {
            initError_ = 2;
            initSchemaVersion_ = model_->version();
            return false;
        }

        // Register ops used by our causal CNN model.
        // Conv1D → Conv2D (TFLite internal), ZeroPadding1D → Pad,
        // BatchNorm fused into conv weights during export, remaining as Mul/Add.
        // Dilated convs (dilation > 1) use SpaceToBatchNd/BatchToSpaceNd.
        // EXPAND_DIMS is inserted by TFLite converter for shape manipulation.
        // If AllocateTensors fails, inspect model ops with:
        //   python -c "import tensorflow as tf; i=tf.lite.Interpreter('model.tflite'); ..."
        static tflite::MicroErrorReporter micro_error_reporter;
        static tflite::MicroMutableOpResolver<14> resolver;
        resolver.AddConv2D();          // Conv1D is implemented as Conv2D internally
        resolver.AddDepthwiseConv2D(); // Depthwise separable conv (v9+ DS-TCN model)
        resolver.AddReshape();
        resolver.AddExpandDims();      // Shape manipulation (TFLite converter inserts)
        resolver.AddLogistic();        // Sigmoid (output layer)
        resolver.AddRelu();            // Separate ReLU after dilated depthwise conv
        resolver.AddQuantize();
        resolver.AddDequantize();
        resolver.AddPad();             // ZeroPadding1D (causal padding)
        resolver.AddMul();             // BatchNorm (if not fused)
        resolver.AddAdd();             // Residual skip connections / BatchNorm bias
        resolver.AddSpaceToBatchNd();  // Dilated conv (dilation > 1)
        resolver.AddBatchToSpaceNd();  // Dilated conv output reshape
        // 14 slots used. If loading a model that needs additional ops,
        // AllocateTensors will fail with error=3 — add the missing op here.

        static tflite::MicroProfiler micro_profiler;
        profiler_ = &micro_profiler;

        static tflite::MicroInterpreter static_interpreter(
            model_, resolver, tensorArena_, TENSOR_ARENA_SIZE,
            &micro_error_reporter, nullptr, &micro_profiler);
        interpreter_ = &static_interpreter;

        TfLiteStatus allocStatus = interpreter_->AllocateTensors();
        if (allocStatus != kTfLiteOk) {
            initError_ = 3;
            return false;
        }
        arenaUsed_ = interpreter_->arena_used_bytes();

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
        unsigned long tQuant0 = micros();
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
        lastQuantUs_ = micros() - tQuant0;

        // Run inference (with timing and per-op profiling)
        if (profiler_) profiler_->ClearEvents();
        unsigned long t0 = micros();
        if (interpreter_->Invoke() != kTfLiteOk) {
            invokeErrors_++;
            return 0.0f;
        }
        lastInferUs_ = micros() - t0;

        // Per-operator profiling: only when enabled via `set nnprofile 1`.
        // Prints [NNPROF] block every 50 inferences (~10s at 5 FPS).
        // Disabled by default to avoid polluting the serial stream / breaking JSON parsers.
        inferCount_++;
        if (profileEnabled_ && inferCount_ % 50 == 0) {
            Serial.print(F("[NNPROF] cnt="));
            Serial.print(inferCount_);
            Serial.print(F(" inv="));
            Serial.print(lastInferUs_);
            Serial.print(F(" q="));
            Serial.print(lastQuantUs_);
            if (profiler_) {
                Serial.print(F(" ticks="));
                Serial.print(profiler_->GetTotalTicks());
            }
            Serial.println();
            if (profiler_) {
                profiler_->Log();
            }
            Serial.println(F("[/NNPROF]"));
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

    /** Enable/disable per-operator profiling output to Serial. */
    void setProfileEnabled(bool enabled) { profileEnabled_ = enabled; }
    bool isProfileEnabled() const { return profileEnabled_; }

    /** Get count of failed Invoke() calls (returns 0.0f silently on failure). */
    uint32_t getInvokeErrors() const { return invokeErrors_; }
    uint32_t getInferCount() const { return inferCount_; }

    /** Print diagnostic info to Serial (call after Serial.begin). */
    void printDiagnostics() const {
        Serial.print(F("[NN] ready="));
        Serial.print(ready_ ? F("yes") : F("no"));
        if (ready_) {
            Serial.print(F(" arena="));
            Serial.print(arenaUsed_);
            Serial.print(F("/"));
            Serial.print(TENSOR_ARENA_SIZE);
            Serial.print(F(" channels="));
            Serial.print(outputChannels_);
            Serial.print(F(" context="));
            Serial.print(contextLen_);
            Serial.print(F(" infer="));
            Serial.print(lastInferUs_ / 1000);
            Serial.print(F("ms quant="));
            Serial.print(lastQuantUs_ / 1000);
            Serial.print(F("ms cnt="));
            Serial.print(inferCount_);
            if (invokeErrors_ > 0) {
                Serial.print(F(" ERRORS="));
                Serial.print(invokeErrors_);
            }
        } else {
            Serial.print(F(" error="));
            Serial.print(initError_);
            if (initError_ == 2) {
                Serial.print(F(" schema="));
                Serial.print(initSchemaVersion_);
            }
        }
        Serial.println();
    }

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

    // Tensor arena — pre-allocated, no dynamic memory.
    // Only exists in NN=1 builds (#ifdef ENABLE_NN_BEAT_ACTIVATION).
    // Measured: v6 BN-fused 14 KB, v7 28 KB. 96 KB covers all deployed models
    // with headroom for future architectures. Non-NN builds pay zero cost.
    static constexpr int TENSOR_ARENA_SIZE = 98304;  // 96 KB
    alignas(16) uint8_t tensorArena_[TENSOR_ARENA_SIZE];

    // Context buffer for sliding window.
    // 5L models (v4/v6) use 128 frames; 7L models (v7/v8) need 256.
    // Runtime contextLen_ is set from the model's actual input shape.
    static constexpr int MAX_CONTEXT = 256;
    float contextBuffer_[MAX_CONTEXT * INPUT_MEL_BANDS];
    int contextLen_ = 0;      // Actual context length from model input shape
    int contextWriteIdx_ = 0; // How many frames we've written so far

    const tflite::Model* model_ = nullptr;
    tflite::MicroInterpreter* interpreter_ = nullptr;
    tflite::MicroProfiler* profiler_ = nullptr;
    TfLiteTensor* input_ = nullptr;
    TfLiteTensor* output_ = nullptr;
    int outputChannels_ = 1;  // 1 = beat only, 2 = beat + downbeat
    float lastDownbeat_ = 0.0f;
    bool ready_ = false;
    bool profileEnabled_ = false;     // Runtime toggle for [NNPROF] serial output
    uint32_t inferCount_ = 0;         // Inference counter for periodic profile
    int initError_ = 0;       // 0=not attempted, 1=null model, 2=schema, 3=alloc
    int initSchemaVersion_ = 0;
    size_t arenaUsed_ = 0;
    unsigned long lastInferUs_ = 0;   // Last Invoke() time in microseconds
    unsigned long lastQuantUs_ = 0;   // Last quantization loop time in microseconds
    uint32_t invokeErrors_ = 0;       // Count of failed Invoke() calls (silent fallback to 0.0)
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
    void setProfileEnabled(bool) {}
    bool isProfileEnabled() const { return false; }
    uint32_t getInvokeErrors() const { return 0; }
    uint32_t getInferCount() const { return 0; }
    void printDiagnostics() const { Serial.println(F("[NN] not compiled")); }
};

#endif // ENABLE_NN_BEAT_ACTIVATION
