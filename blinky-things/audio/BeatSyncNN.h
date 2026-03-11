#pragma once

// ============================================================================
// BeatSyncNN — TFLite Micro inference for beat-synchronous FC classifier
// ============================================================================
//
// Runs at beat rate (~2 Hz) instead of frame rate (62.5 Hz).
// Input: flattened last N beats of accumulated features from SpectralAccumulator
// Output: downbeat probability (Phase A), + confidence/tempo/phase (Phase B/C)
//
// Architecture: FC-only (316→32→16→1), ~10.7K params, ~10.4 KB INT8
// Inference: ~83µs on Cortex-M4F @ 64 MHz (negligible)
// Memory: ~4 KB tensor arena + 2.5 KB beat history (4 beats × 79 floats × 4 bytes)
//         + ~0.3 KB SpectralAccumulator (managed externally)
//
// Training uses z-score normalized features, but the export script folds the
// normalization (mean/std) into the first FC layer weights. The exported model
// accepts raw features directly — no normalization needed in firmware.
//
// Enable via: compile with ENABLE_NN_BEAT_ACTIVATION, model in beat_sync_model_data.h
// ============================================================================

#ifdef ENABLE_NN_BEAT_ACTIVATION

#include <TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "beat_sync_model_data.h"
#include "SpectralAccumulator.h"

class BeatSyncNN {
public:
    static constexpr int N_BEATS = 4;
    static constexpr int FEATURES_PER_BEAT = SpectralAccumulator::FEATURES_PER_BEAT;  // 79
    static constexpr int INPUT_DIM = N_BEATS * FEATURES_PER_BEAT;  // 316

    bool begin() {
        if (ready_) return true;

        // Only initialize if we have a real model (not the 4-byte placeholder stub)
        if (beat_sync_model_data_len < 100) {
            initError_ = 4;  // Placeholder model
            return false;
        }

        model_ = tflite::GetModel(beat_sync_model_data);
        if (model_ == nullptr) {
            initError_ = 1;
            return false;
        }
        if (model_->version() != TFLITE_SCHEMA_VERSION) {
            initError_ = 2;
            return false;
        }

        // FC-only model needs very few ops: FullyConnected + Logistic (sigmoid)
        // Quantize/Dequantize for INT8 I/O, Reshape for shape manipulation
        static tflite::MicroErrorReporter error_reporter;
        static tflite::MicroMutableOpResolver<6> resolver;
        resolver.AddFullyConnected();
        resolver.AddLogistic();        // Sigmoid output
        resolver.AddRelu();            // Hidden layer activations
        resolver.AddQuantize();
        resolver.AddDequantize();
        resolver.AddReshape();

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

        // Verify input dimension matches expected
        int inputSize = 1;
        for (int i = 0; i < input_->dims->size; i++) {
            inputSize *= input_->dims->data[i];
        }
        if (inputSize != INPUT_DIM) {
            initError_ = 5;  // Input dimension mismatch
            return false;
        }

        // Zero-fill beat history
        memset(beatHistory_, 0, sizeof(beatHistory_));
        beatHistoryCount_ = 0;
        beatHistoryIdx_ = 0;

        ready_ = true;
        return true;
    }

    /**
     * Push one beat's features into the history ring buffer.
     * Call at each beat fire with features from SpectralAccumulator::getFeatures().
     *
     * @param features: FEATURES_PER_BEAT floats from SpectralAccumulator
     */
    void pushBeat(const float* features) {
        memcpy(beatHistory_[beatHistoryIdx_], features, FEATURES_PER_BEAT * sizeof(float));
        beatHistoryIdx_ = (beatHistoryIdx_ + 1) % N_BEATS;
        if (beatHistoryCount_ < N_BEATS) beatHistoryCount_++;
    }

    /**
     * Run inference on the last N_BEATS of accumulated features.
     * Returns downbeat probability [0, 1].
     * Only valid after pushBeat() has been called at least N_BEATS times.
     */
    float infer() {
        if (!ready_ || beatHistoryCount_ < N_BEATS) return 0.0f;

        // Build input: flatten N_BEATS in chronological order from ring buffer
        // beatHistoryIdx_ points to the NEXT write position, so oldest is at beatHistoryIdx_
        float inputBuffer[INPUT_DIM];
        for (int b = 0; b < N_BEATS; b++) {
            int srcIdx = (beatHistoryIdx_ + b) % N_BEATS;  // oldest first
            memcpy(&inputBuffer[b * FEATURES_PER_BEAT],
                   beatHistory_[srcIdx],
                   FEATURES_PER_BEAT * sizeof(float));
        }

        // Quantize and copy to input tensor
        if (input_->type == kTfLiteInt8) {
            float scale = input_->params.scale;
            int32_t zero_point = input_->params.zero_point;
            int8_t* input_data = input_->data.int8;
            for (int i = 0; i < INPUT_DIM; i++) {
                int32_t q = static_cast<int32_t>(inputBuffer[i] / scale + zero_point);
                if (q < -128) q = -128;
                if (q > 127) q = 127;
                input_data[i] = static_cast<int8_t>(q);
            }
        } else {
            memcpy(input_->data.f, inputBuffer, INPUT_DIM * sizeof(float));
        }

        // Run inference
        unsigned long t0 = micros();
        if (interpreter_->Invoke() != kTfLiteOk) {
            invokeErrors_++;
            return 0.0f;
        }
        lastInferUs_ = micros() - t0;
        inferCount_++;

        // Extract downbeat probability (single sigmoid output)
        float downbeat;
        if (output_->type == kTfLiteInt8) {
            float scale = output_->params.scale;
            int32_t zero_point = output_->params.zero_point;
            downbeat = (output_->data.int8[0] - zero_point) * scale;
        } else {
            downbeat = output_->data.f[0];
        }

        if (downbeat < 0.0f) downbeat = 0.0f;
        if (downbeat > 1.0f) downbeat = 1.0f;
        return downbeat;
    }

    bool isReady() const { return ready_; }
    int getBeatHistoryCount() const { return beatHistoryCount_; }
    uint32_t getInferCount() const { return inferCount_; }
    uint32_t getInvokeErrors() const { return invokeErrors_; }
    unsigned long getLastInferUs() const { return lastInferUs_; }
    size_t getArenaUsed() const { return arenaUsed_; }

    void printDiagnostics() const {
        Serial.print(F("[BeatSyncNN] ready="));
        Serial.print(ready_ ? F("yes") : F("no"));
        if (ready_) {
            Serial.print(F(" arena="));
            Serial.print(arenaUsed_);
            Serial.print(F("/"));
            Serial.print(TENSOR_ARENA_SIZE);
            Serial.print(F(" beats="));
            Serial.print(beatHistoryCount_);
            Serial.print(F("/"));
            Serial.print(N_BEATS);
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
    // Tensor arena: FC-only model needs ~4 KB
    // Generous at 8 KB to accommodate quantization buffers
    static constexpr int TENSOR_ARENA_SIZE = 8192;
    alignas(16) uint8_t tensorArena_[TENSOR_ARENA_SIZE];

    // Beat history ring buffer: last N_BEATS feature vectors
    // Memory: 4 × 79 × 4 = 1,264 bytes
    float beatHistory_[N_BEATS][FEATURES_PER_BEAT];
    int beatHistoryIdx_ = 0;
    int beatHistoryCount_ = 0;

    const tflite::Model* model_ = nullptr;
    tflite::MicroInterpreter* interpreter_ = nullptr;
    TfLiteTensor* input_ = nullptr;
    TfLiteTensor* output_ = nullptr;
    bool ready_ = false;
    int initError_ = 0;  // 0=not attempted, 1=null model, 2=schema, 3=alloc, 4=placeholder, 5=dim mismatch
    size_t arenaUsed_ = 0;
    unsigned long lastInferUs_ = 0;
    uint32_t inferCount_ = 0;
    uint32_t invokeErrors_ = 0;
};

#else

// Stub when NN is not compiled in
class BeatSyncNN {
public:
    static constexpr int N_BEATS = 4;
    static constexpr int FEATURES_PER_BEAT = 79;
    static constexpr int INPUT_DIM = N_BEATS * FEATURES_PER_BEAT;
    bool begin() { return false; }
    void pushBeat(const float*) {}
    float infer() { return 0.0f; }
    bool isReady() const { return false; }
    int getBeatHistoryCount() const { return 0; }
    uint32_t getInferCount() const { return 0; }
    uint32_t getInvokeErrors() const { return 0; }
    unsigned long getLastInferUs() const { return 0; }
    size_t getArenaUsed() const { return 0; }
    void printDiagnostics() const { Serial.println(F("[BeatSyncNN] not compiled")); }
};

#endif // ENABLE_NN_BEAT_ACTIVATION
