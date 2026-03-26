#pragma once

// ============================================================================
// FrameOnsetNN — Single-model TFLite Micro inference for onset detection
// ============================================================================
//
// Detects acoustic onsets (kicks, snares) from mel spectrograms.
// With a 144ms receptive field (Conv1D W16, k=5x2) it can only detect
// local transients — it cannot distinguish on-beat from off-beat onsets.
//
// Current model: Conv1D W16, single output channel (onset activation).
// Supports up to 4 output channels for future multi-task models.
//
// Output is used for:
//   - Visual pulse detection (sparks, flashes on kicks/snares)
//   - PLP source comparison (Fourier tempogram evaluates NN onset alongside flux/bass)
//   - Energy synthesis (ODF peak-hold blend)
//
// NOT used for BPM estimation — spectral flux drives ACF tempo instead.
//
// Consumes raw mel bands from SharedSpectralAnalysis::getRawMelBands().
// Falls back to mic energy if model fails to load.
//
// ============================================================================

#include <TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Only ONE model header can be active per build. To deploy a versioned model:
//   cp blinky-things/audio/frame_onset_model_data_v3.h blinky-things/audio/frame_onset_model_data.h
// The export_tflite.py script writes directly to this path by default.
#include "frame_onset_model_data.h"

class FrameOnsetNN {
public:
    static constexpr int INPUT_MEL_BANDS = 26;     // Raw mel bands from SharedSpectralAnalysis
    static constexpr int MAX_INPUT_FEATURES = 52;  // Mel + delta (model auto-detects)

    /**
     * Initialize TFLite interpreter and allocate tensors.
     * Call once at startup only — reinit not supported.
     * Returns true if model initialized successfully.
     */
    bool begin() {
        if (ready_) return true;

        initResolver();

        if (frame_onset_model_data_len < 100) {
            initError_ = 4;  // Placeholder model
            return false;
        }

        model_ = tflite::GetModel(frame_onset_model_data);
        if (model_ == nullptr) { initError_ = 1; return false; }
        if (model_->version() != TFLITE_SCHEMA_VERSION) { initError_ = 2; return false; }

        // NOTE: static storage — only one FrameOnsetNN instance supported.
        alignas(alignof(tflite::MicroInterpreter))
        static uint8_t interpStorage[sizeof(tflite::MicroInterpreter)];
        interpreter_ = new(interpStorage) tflite::MicroInterpreter(
            model_, resolver_, arena_, ARENA_SIZE, &errorReporter_);

        TfLiteStatus allocStatus = interpreter_->AllocateTensors();
        if (allocStatus != kTfLiteOk) { initError_ = 3; return false; }
        arenaUsed_ = interpreter_->arena_used_bytes();

        input_ = interpreter_->input(0);
        output_ = interpreter_->output(0);

        // Detect window size from input shape
        int inputSize = 1;
        for (int i = 0; i < input_->dims->size; i++) {
            inputSize *= input_->dims->data[i];
        }
        // Detect input features per frame: 26 (mel only) or 52 (mel + delta).
        // Model input shape is [1, windowFrames, featuresPerFrame].
        int featuresPerFrame = input_->dims->data[input_->dims->size - 1];
        if (featuresPerFrame != INPUT_MEL_BANDS && featuresPerFrame != INPUT_MEL_BANDS * 2) {
            initError_ = 5; return false;
        }
        useDelta_ = (featuresPerFrame == INPUT_MEL_BANDS * 2);
        inputFeatures_ = featuresPerFrame;
        windowFrames_ = inputSize / featuresPerFrame;
        if (windowFrames_ > MAX_WINDOW_FRAMES) { initError_ = 6; return false; }

        // Detect output shape
        int totalOutputs = 1;
        for (int i = 0; i < output_->dims->size; i++) {
            totalOutputs *= output_->dims->data[i];
        }
        outputChannels_ = output_->dims->data[output_->dims->size - 1];
        if (outputChannels_ < 1 || outputChannels_ > 4 || totalOutputs < outputChannels_) {
            initError_ = 7; return false;
        }
        // Assumes output shape [1, T, C] — last C elements are the final frame's activations
        outputOffset_ = totalOutputs - outputChannels_;

        memset(windowBuffer_, 0, sizeof(windowBuffer_));
        windowFilled_ = 0;
        ready_ = true;
        return true;
    }

    /**
     * Feed one frame of mel bands, run inference, return onset activation [0,1].
     * Downbeat result cached — access via getLastDownbeat().
     */
    float infer(const float* melBands) {
        if (!ready_) return 0.0f;

        // Build the feature vector for this frame: mel bands + optional delta
        float frameFeatures[MAX_INPUT_FEATURES];
        memcpy(frameFeatures, melBands, INPUT_MEL_BANDS * sizeof(float));
        if (useDelta_) {
            // Delta = mel[t] - mel[t-1]. First frame gets zero delta.
            for (int i = 0; i < INPUT_MEL_BANDS; i++) {
                frameFeatures[INPUT_MEL_BANDS + i] = melBands[i] - prevMel_[i];
            }
            memcpy(prevMel_, melBands, INPUT_MEL_BANDS * sizeof(float));
        }

        // Sliding window: push new frame (using inputFeatures_ width)
        if (windowFilled_ < windowFrames_) {
            memcpy(&windowBuffer_[windowFilled_ * inputFeatures_],
                   frameFeatures, inputFeatures_ * sizeof(float));
            windowFilled_++;
        } else {
            memmove(windowBuffer_, windowBuffer_ + inputFeatures_,
                    (windowFrames_ - 1) * inputFeatures_ * sizeof(float));
            memcpy(&windowBuffer_[(windowFrames_ - 1) * inputFeatures_],
                   frameFeatures, inputFeatures_ * sizeof(float));
        }

        if (windowFilled_ < windowFrames_) return 0.0f;

        // Quantize input
        int totalInputs = windowFrames_ * inputFeatures_;
        if (input_->type == kTfLiteInt8) {
            float scale = input_->params.scale;
            if (scale <= 0.0f) { invokeErrors_++; return 0.0f; }
            int32_t zero_point = input_->params.zero_point;
            int8_t* input_data = input_->data.int8;
            for (int i = 0; i < totalInputs; i++) {
                int32_t q = static_cast<int32_t>(roundf(windowBuffer_[i] / scale) + zero_point);
                if (q < -128) q = -128;
                if (q > 127) q = 127;
                input_data[i] = static_cast<int8_t>(q);
            }
        } else {
            memcpy(input_->data.f, windowBuffer_, totalInputs * sizeof(float));
        }

        unsigned long t0 = micros();
        if (interpreter_->Invoke() != kTfLiteOk) {
            invokeErrors_++;
            return 0.0f;
        }
        lastInferUs_ = micros() - t0;
        inferCount_++;

        if (profileEnabled_ && (inferCount_ % 100 == 0)) {
            Serial.print(F("[NNPROF] cnt="));
            Serial.print(inferCount_);
            Serial.print(F(" infer="));
            Serial.print(lastInferUs_);
            Serial.print(F("us arena="));
            Serial.print(arenaUsed_);
            Serial.print(F("/"));
            Serial.println(ARENA_SIZE);
        }

        // Extract outputs
        lastOnset_ = extractOutput(0);
        if (outputChannels_ >= 2) {
            lastDownbeat_ = extractOutput(1);
        }

        return lastOnset_;
    }

    // --- Status ---

    bool isReady() const { return ready_; }
    bool hasDownbeatOutput() const { return ready_ && outputChannels_ >= 2; }

    // --- Output accessors ---

    float getLastOnset() const { return lastOnset_; }
    float getLastDownbeat() const { return lastDownbeat_; }

    // --- Profiling ---

    void setProfileEnabled(bool enabled) { profileEnabled_ = enabled; }
    bool isProfileEnabled() const { return profileEnabled_; }
    unsigned long getLastInferUs() const { return lastInferUs_; }
    uint32_t getInferCount() const { return inferCount_; }
    uint32_t getInvokeErrors() const { return invokeErrors_; }

    /** Print diagnostic info to Serial. */
    void printDiagnostics() const {
        Serial.print(F("[NN] ready="));
        Serial.print(ready_ ? F("yes") : F("no"));
        if (ready_) {
            Serial.print(F(" arena="));
            Serial.print(arenaUsed_);
            Serial.print(F("/"));
            Serial.print(ARENA_SIZE);
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
            Serial.print(F("\n  iscale="));
            Serial.print(input_->params.scale, 8);
            Serial.print(F(" izp="));
            Serial.print(input_->params.zero_point);
            Serial.print(F(" oscale="));
            Serial.print(output_->params.scale, 8);
            Serial.print(F(" ozp="));
            Serial.print(output_->params.zero_point);
            Serial.print(F("\n  onset="));
            Serial.print(lastOnset_, 4);
            if (outputChannels_ >= 2) {
                Serial.print(F(" db="));
                Serial.print(lastDownbeat_, 4);
            }
            if (input_->type == kTfLiteInt8 && windowFilled_ >= windowFrames_) {
                int lastFrameStart = (windowFrames_ - 1) * inputFeatures_;
                Serial.print(F("\n  last_mel=["));
                Serial.print(windowBuffer_[lastFrameStart], 4);
                Serial.print(F(","));
                Serial.print(windowBuffer_[lastFrameStart + 1], 4);
                Serial.print(F(",...,"));
                Serial.print(windowBuffer_[lastFrameStart + INPUT_MEL_BANDS - 1], 4);
                Serial.print(F("]"));
                if (useDelta_) {
                    Serial.print(F(" delta=["));
                    Serial.print(windowBuffer_[lastFrameStart + INPUT_MEL_BANDS], 4);
                    Serial.print(F(",...,"));
                    Serial.print(windowBuffer_[lastFrameStart + inputFeatures_ - 1], 4);
                    Serial.print(F("]"));
                }
            }
        } else {
            Serial.print(F(" error="));
            Serial.print(initError_);
        }
        Serial.println();
    }

private:
    // Op resolver — supports FC, Conv1D, and Conv1D+sum_head models:
    // FC: FullyConnected, Logistic, Quantize, Dequantize, Reshape
    // Conv1D: Conv2D, Pad, Reshape, ExpandDims, Logistic
    // Sum head adds: Mul, StridedSlice, Concatenation, extra Quantize
    static constexpr int OP_RESOLVER_SLOTS = 12;

    tflite::MicroErrorReporter errorReporter_;
    tflite::MicroMutableOpResolver<OP_RESOLVER_SLOTS> resolver_;

    bool resolverInited_ = false;

    void initResolver() {
        if (resolverInited_) return;
        resolver_.AddFullyConnected();  // FC models
        resolver_.AddConv2D();          // Conv1D mapped to Conv2D
        resolver_.AddPad();             // Causal padding (ZeroPadding1D)
        resolver_.AddReshape();         // Tensor shape conversion
        resolver_.AddExpandDims();      // 1D→2D input expansion
        resolver_.AddLogistic();        // Sigmoid activation
        resolver_.AddMul();             // Sum head: beat * sigmoid(db_logit)
        resolver_.AddStridedSlice();    // Sum head: split channels
        resolver_.AddConcatenation();   // Sum head: join beat + downbeat
        resolver_.AddQuantize();
        resolver_.AddDequantize();
        resolverInited_ = true;
    }

    float extractOutput(int channel) const {
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

    // --- Model state ---

    static constexpr int ARENA_SIZE = 32768;          // 32 KB (Conv1D W64 measured 7340 — headroom for model updates)
    static constexpr int MAX_WINDOW_FRAMES = 64;      // Max 64 frames (1.024s) — increase if a wider model is used

    alignas(16) uint8_t arena_[ARENA_SIZE];
    float windowBuffer_[MAX_WINDOW_FRAMES * MAX_INPUT_FEATURES];  // 13 KB max (64 * 52 * 4)
    float prevMel_[INPUT_MEL_BANDS] = {0};      // Previous frame mel bands (for delta computation)
    int windowFrames_ = 0;
    int windowFilled_ = 0;
    int inputFeatures_ = INPUT_MEL_BANDS;       // 26 (mel) or 52 (mel+delta), auto-detected from model
    bool useDelta_ = false;                      // True when model expects 52-dim input

    const tflite::Model* model_ = nullptr;
    tflite::MicroInterpreter* interpreter_ = nullptr;
    TfLiteTensor* input_ = nullptr;
    TfLiteTensor* output_ = nullptr;
    int outputChannels_ = 1;
    int outputOffset_ = 0;
    float lastOnset_ = 0.0f;
    float lastDownbeat_ = 0.0f;
    bool ready_ = false;
    int initError_ = 0;
    size_t arenaUsed_ = 0;
    unsigned long lastInferUs_ = 0;
    uint32_t inferCount_ = 0;
    uint32_t invokeErrors_ = 0;
    bool profileEnabled_ = false;
};
