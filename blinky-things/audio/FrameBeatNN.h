#pragma once

// ============================================================================
// FrameBeatNN — Dual-model TFLite Micro inference for beat detection
// ============================================================================
//
// Two specialized models replace the single FC/Conv1D:
//
//   OnsetNN:  Conv1D, W8 (128ms), 1 output (onset activation)
//             Runs every frame (62.5 Hz), <1ms inference.
//             Drives ODF → CBSS beat tracking + pulse detection.
//
//   RhythmNN: Conv1D+Pool, W192 (3.07s), 2 outputs (beat + downbeat)
//             Runs every 2nd frame (31.25 Hz), <8ms inference.
//             Provides downbeat activation for bar structure.
//
// Both consume raw mel bands from SharedSpectralAnalysis::getRawMelBands().
// Each model has its own sliding window buffer and tensor arena.
// Falls back gracefully: if either model fails to load, its outputs are 0.
// If both fail, AudioController falls back to mic energy as ODF.
//
// Potential optimization: run RhythmNN on onset spikes instead of fixed
// schedule. When OnsetNN fires a strong activation, immediately trigger
// RhythmNN inference so its output is freshest at beat fire time.
// Not implemented yet — fixed 2-frame schedule is simpler and sufficient.
//
// ============================================================================

#include <TensorFlowLite.h>
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include "onset_model_data.h"
#include "rhythm_model_data.h"

class FrameBeatNN {
public:
    static constexpr int INPUT_MEL_BANDS = 26;

    /**
     * Initialize both TFLite interpreters and allocate tensors.
     * Must be called once at startup. Either model may fail independently.
     * Returns true if at least OnsetNN initialized successfully.
     */
    bool begin() {
        if (onsetReady_ || rhythmReady_) return onsetReady_;

        initSharedResolver();

        // Initialize OnsetNN
        if (onset_model_data_len >= 100) {
            onsetReady_ = initModel(
                onset_model_data, onset_model_data_len,
                &onsetModel_, &onsetInterpreter_,
                onsetArena_, ONSET_ARENA_SIZE,
                &onsetInput_, &onsetOutput_,
                &onsetWindowFrames_, &onsetOutputChannels_, &onsetOutputOffset_,
                &onsetArenaUsed_, &onsetInitError_
            );
        } else {
            onsetInitError_ = 4;  // Placeholder model
        }

        // Initialize RhythmNN
        if (rhythm_model_data_len >= 100) {
            rhythmReady_ = initModel(
                rhythm_model_data, rhythm_model_data_len,
                &rhythmModel_, &rhythmInterpreter_,
                rhythmArena_, RHYTHM_ARENA_SIZE,
                &rhythmInput_, &rhythmOutput_,
                &rhythmWindowFrames_, &rhythmOutputChannels_, &rhythmOutputOffset_,
                &rhythmArenaUsed_, &rhythmInitError_
            );
        } else {
            rhythmInitError_ = 4;  // Placeholder model
        }

        // Zero-fill window buffers
        memset(onsetWindowBuffer_, 0, sizeof(onsetWindowBuffer_));
        memset(rhythmWindowBuffer_, 0, sizeof(rhythmWindowBuffer_));
        onsetWindowFilled_ = 0;
        rhythmWindowFilled_ = 0;

        return onsetReady_;
    }

    /**
     * Feed one frame of mel bands to OnsetNN. Runs inference every call.
     * Returns onset activation in [0, 1].
     * Also pushes the frame into RhythmNN's window buffer (no inference).
     */
    float inferOnset(const float* melBands) {
        // Always buffer into RhythmNN's window (even if RhythmNN isn't ready,
        // keeps the window filled for when inferRhythm() is called).
        // Use actual model window size if loaded, otherwise max buffer size.
        int rhythmBufFrames = (rhythmWindowFrames_ > 0)
            ? rhythmWindowFrames_ : RHYTHM_MAX_WINDOW_FRAMES;
        pushFrame(rhythmWindowBuffer_, rhythmWindowFilled_,
                  rhythmBufFrames, melBands);

        if (!onsetReady_) return 0.0f;

        // Use actual model window size (not buffer max) so the sliding window
        // keeps the most recent N frames aligned at positions [0..N-1].
        pushFrame(onsetWindowBuffer_, onsetWindowFilled_,
                  onsetWindowFrames_, melBands);

        if (onsetWindowFilled_ < onsetWindowFrames_) return 0.0f;

        float beat = runInference(
            onsetInput_, onsetOutput_, onsetInterpreter_,
            onsetWindowBuffer_, onsetWindowFrames_,
            onsetOutputChannels_, onsetOutputOffset_,
            onsetInferCount_, onsetInvokeErrors_, onsetLastInferUs_,
            F("onset")
        );

        onsetLastBeat_ = beat;
        return beat;
    }

    /**
     * Run RhythmNN inference on the buffered mel window.
     * Call every 2nd frame from AudioController for 32ms max staleness.
     * Returns true if inference ran (window full + model ready).
     * Results available via getLastDownbeat() / getLastRhythmBeat().
     */
    bool inferRhythm() {
        if (!rhythmReady_) return false;
        if (rhythmWindowFilled_ < rhythmWindowFrames_) return false;

        float beat = runInference(
            rhythmInput_, rhythmOutput_, rhythmInterpreter_,
            rhythmWindowBuffer_, rhythmWindowFrames_,
            rhythmOutputChannels_, rhythmOutputOffset_,
            rhythmInferCount_, rhythmInvokeErrors_, rhythmLastInferUs_,
            F("rhythm")
        );

        rhythmLastBeat_ = beat;
        rhythmLastDownbeat_ = (rhythmOutputChannels_ >= 2)
            ? extractOutput(rhythmOutput_, rhythmOutputChannels_, rhythmOutputOffset_, 1)
            : 0.0f;

        return true;
    }

    // --- Status ---

    /** True if OnsetNN loaded successfully (primary ODF source). */
    bool isReady() const { return onsetReady_; }

    /** True if RhythmNN loaded successfully (downbeat detection). */
    bool isRhythmReady() const { return rhythmReady_; }

    /** Whether RhythmNN has a downbeat output head. */
    bool hasDownbeatOutput() const { return rhythmReady_ && rhythmOutputChannels_ >= 2; }

    // --- Output accessors ---

    float getLastBeat() const { return onsetLastBeat_; }
    float getLastDownbeat() const { return rhythmLastDownbeat_; }
    float getLastRhythmBeat() const { return rhythmLastBeat_; }

    // --- Profiling ---

    void setProfileEnabled(bool enabled) { profileEnabled_ = enabled; }
    bool isProfileEnabled() const { return profileEnabled_; }
    unsigned long getOnsetInferUs() const { return onsetLastInferUs_; }
    unsigned long getRhythmInferUs() const { return rhythmLastInferUs_; }
    uint32_t getOnsetInferCount() const { return onsetInferCount_; }
    uint32_t getRhythmInferCount() const { return rhythmInferCount_; }
    uint32_t getOnsetInvokeErrors() const { return onsetInvokeErrors_; }
    uint32_t getRhythmInvokeErrors() const { return rhythmInvokeErrors_; }

    /** Print diagnostic info for both models to Serial. */
    void printDiagnostics() const {
        // OnsetNN
        Serial.print(F("[OnsetNN] ready="));
        Serial.print(onsetReady_ ? F("yes") : F("no"));
        if (onsetReady_) {
            printModelDiag(
                onsetArenaUsed_, ONSET_ARENA_SIZE, onsetWindowFrames_,
                onsetOutputChannels_,
                onsetLastInferUs_, onsetInferCount_, onsetInvokeErrors_,
                onsetInput_, onsetOutput_, onsetOutputOffset_,
                onsetLastBeat_, 0.0f,
                onsetWindowBuffer_, onsetWindowFilled_
            );
        } else {
            Serial.print(F(" error="));
            Serial.print(onsetInitError_);
        }
        Serial.println();

        // RhythmNN
        Serial.print(F("[RhythmNN] ready="));
        Serial.print(rhythmReady_ ? F("yes") : F("no"));
        if (rhythmReady_) {
            printModelDiag(
                rhythmArenaUsed_, RHYTHM_ARENA_SIZE, rhythmWindowFrames_,
                rhythmOutputChannels_,
                rhythmLastInferUs_, rhythmInferCount_, rhythmInvokeErrors_,
                rhythmInput_, rhythmOutput_, rhythmOutputOffset_,
                rhythmLastBeat_, rhythmLastDownbeat_,
                rhythmWindowBuffer_, rhythmWindowFilled_
            );
        } else {
            Serial.print(F(" error="));
            Serial.print(rhythmInitError_);
        }
        Serial.println();
    }

private:
    // --- Shared op resolver (both models use the same ops) ---

    static constexpr int OP_RESOLVER_SLOTS = 10;

    // Returns pointers to shared statics (C++11-safe via static locals).
    // Both models use the same resolver and error reporter.
    struct SharedState {
        tflite::MicroErrorReporter errorReporter;
        tflite::MicroMutableOpResolver<OP_RESOLVER_SLOTS> resolver;
        bool inited = false;
    };

    static SharedState& getShared() {
        static SharedState s;
        return s;
    }

    void initSharedResolver() {
        SharedState& s = getShared();
        if (s.inited) return;
        s.resolver.AddConv2D();          // Conv1D mapped to Conv2D
        s.resolver.AddPad();             // Causal padding (ZeroPadding1D)
        s.resolver.AddReshape();         // 1D↔2D tensor conversion
        s.resolver.AddExpandDims();      // 1D→2D input expansion
        s.resolver.AddLogistic();        // Sigmoid output
        s.resolver.AddAveragePool2D();   // AvgPool1D mapped to AvgPool2D
        s.resolver.AddFullyConnected();  // RhythmNN FC head
        s.resolver.AddQuantize();
        s.resolver.AddDequantize();
        s.inited = true;
    }

    // --- Model initialization (shared logic) ---

    bool initModel(
        const unsigned char* modelData, int modelDataLen,
        const tflite::Model** model, tflite::MicroInterpreter** interpreter,
        uint8_t* arena, int arenaSize,
        TfLiteTensor** input, TfLiteTensor** output,
        int* windowFrames, int* outputChannels, int* outputOffset,
        size_t* arenaUsed, int* initError
    ) {
        *model = tflite::GetModel(modelData);
        if (*model == nullptr) { *initError = 1; return false; }
        if ((*model)->version() != TFLITE_SCHEMA_VERSION) { *initError = 2; return false; }

        // Each model gets its own interpreter (separate arena, separate state).
        // MicroInterpreter is placement-new'd into the provided buffer.
        SharedState& s = getShared();
        static tflite::MicroInterpreter* interpreters[2] = {nullptr, nullptr};
        alignas(alignof(tflite::MicroInterpreter))
        static uint8_t interpStorage[2][sizeof(tflite::MicroInterpreter)];
        int slot = (arena == rhythmArena_) ? 1 : 0;

        // cppcheck-suppress constStatement -- placement new, not unused access
        interpreters[slot] = new(interpStorage[slot]) tflite::MicroInterpreter(
            *model, s.resolver, arena, arenaSize, &s.errorReporter);
        *interpreter = interpreters[slot];

        TfLiteStatus allocStatus = (*interpreter)->AllocateTensors();
        if (allocStatus != kTfLiteOk) { *initError = 3; return false; }
        *arenaUsed = (*interpreter)->arena_used_bytes();

        *input = (*interpreter)->input(0);
        *output = (*interpreter)->output(0);

        // Detect window size from input shape
        int inputSize = 1;
        for (int i = 0; i < (*input)->dims->size; i++) {
            inputSize *= (*input)->dims->data[i];
        }
        if (inputSize < INPUT_MEL_BANDS || inputSize % INPUT_MEL_BANDS != 0) {
            *initError = 5; return false;
        }
        *windowFrames = inputSize / INPUT_MEL_BANDS;
        int maxWindow = (arena == rhythmArena_) ? RHYTHM_MAX_WINDOW_FRAMES : ONSET_MAX_WINDOW_FRAMES;
        if (*windowFrames > maxWindow) { *initError = 6; return false; }

        // Detect output shape
        int totalOutputs = 1;
        for (int i = 0; i < (*output)->dims->size; i++) {
            totalOutputs *= (*output)->dims->data[i];
        }
        *outputChannels = (*output)->dims->data[(*output)->dims->size - 1];
        if (*outputChannels < 1 || *outputChannels > 4 || totalOutputs < *outputChannels) {
            *initError = 7; return false;
        }
        *outputOffset = totalOutputs - *outputChannels;

        return true;
    }

    // --- Window management ---

    static void pushFrame(float* buffer, int& filled, int maxFrames, const float* melBands) {
        if (filled < maxFrames) {
            memcpy(&buffer[filled * INPUT_MEL_BANDS], melBands, INPUT_MEL_BANDS * sizeof(float));
            filled++;
        } else {
            memmove(buffer, buffer + INPUT_MEL_BANDS,
                    (maxFrames - 1) * INPUT_MEL_BANDS * sizeof(float));
            memcpy(&buffer[(maxFrames - 1) * INPUT_MEL_BANDS],
                   melBands, INPUT_MEL_BANDS * sizeof(float));
        }
    }

    // --- Inference (shared logic) ---

    float runInference(
        TfLiteTensor* input, TfLiteTensor* output,
        tflite::MicroInterpreter* interpreter,
        const float* windowBuffer, int windowFrames,
        int outputChannels, int outputOffset,
        uint32_t& inferCount, uint32_t& invokeErrors, unsigned long& lastInferUs,
        const __FlashStringHelper* modelName = nullptr
    ) {
        int totalInputs = windowFrames * INPUT_MEL_BANDS;

        // Quantize input
        if (input->type == kTfLiteInt8) {
            float scale = input->params.scale;
            if (scale <= 0.0f) { invokeErrors++; return 0.0f; }
            int32_t zero_point = input->params.zero_point;
            int8_t* input_data = input->data.int8;
            for (int i = 0; i < totalInputs; i++) {
                int32_t q = static_cast<int32_t>(windowBuffer[i] / scale + zero_point);
                if (q < -128) q = -128;
                if (q > 127) q = 127;
                input_data[i] = static_cast<int8_t>(q);
            }
        } else {
            memcpy(input->data.f, windowBuffer, totalInputs * sizeof(float));
        }

        unsigned long t0 = micros();
        if (interpreter->Invoke() != kTfLiteOk) {
            invokeErrors++;
            return 0.0f;
        }
        lastInferUs = micros() - t0;
        inferCount++;

        if (profileEnabled_ && (inferCount % 100 == 0)) {
            Serial.print(F("[NNPROF:"));
            if (modelName) Serial.print(modelName);
            Serial.print(F("] cnt="));
            Serial.print(inferCount);
            Serial.print(F(" infer="));
            Serial.print(lastInferUs);
            Serial.println(F("us"));
        }

        return extractOutput(output, outputChannels, outputOffset, 0);
    }

    static float extractOutput(TfLiteTensor* output, int outputChannels, int outputOffset, int channel) {
        if (channel < 0 || channel >= outputChannels) return 0.0f;
        int idx = outputOffset + channel;
        float value;
        if (output->type == kTfLiteInt8) {
            float scale = output->params.scale;
            int32_t zero_point = output->params.zero_point;
            value = (output->data.int8[idx] - zero_point) * scale;
        } else {
            value = output->data.f[idx];
        }
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        return value;
    }

    // --- Diagnostics helper ---

    void printModelDiag(
        size_t arenaUsed, int arenaSize, int windowFrames, int outputChannels,
        unsigned long lastInferUs, uint32_t inferCount, uint32_t invokeErrors,
        TfLiteTensor* input, TfLiteTensor* output, int outputOffset,
        float lastBeat, float lastDownbeat,
        const float* windowBuffer, int windowFilled
    ) const {
        Serial.print(F(" arena="));
        Serial.print(arenaUsed);
        Serial.print(F("/"));
        Serial.print(arenaSize);
        Serial.print(F(" window="));
        Serial.print(windowFrames);
        Serial.print(F(" outputs="));
        Serial.print(outputChannels);
        Serial.print(F(" infer="));
        Serial.print(lastInferUs);
        Serial.print(F("us cnt="));
        Serial.print(inferCount);
        if (invokeErrors > 0) {
            Serial.print(F(" ERRORS="));
            Serial.print(invokeErrors);
        }
        Serial.print(F("\n  iscale="));
        Serial.print(input->params.scale, 8);
        Serial.print(F(" izp="));
        Serial.print(input->params.zero_point);
        Serial.print(F(" oscale="));
        Serial.print(output->params.scale, 8);
        Serial.print(F(" ozp="));
        Serial.print(output->params.zero_point);
        Serial.print(F("\n  beat="));
        Serial.print(lastBeat, 4);
        if (outputChannels >= 2) {
            Serial.print(F(" db="));
            Serial.print(lastDownbeat, 4);
        }
        if (input->type == kTfLiteInt8 && windowFilled >= windowFrames) {
            int lastFrameStart = (windowFrames - 1) * INPUT_MEL_BANDS;
            Serial.print(F("\n  last_mel=["));
            Serial.print(windowBuffer[lastFrameStart], 4);
            Serial.print(F(","));
            Serial.print(windowBuffer[lastFrameStart + 1], 4);
            Serial.print(F(",...,"));
            Serial.print(windowBuffer[lastFrameStart + INPUT_MEL_BANDS - 1], 4);
            Serial.print(F("]"));
        }
    }

    // --- OnsetNN state ---

    static constexpr int ONSET_ARENA_SIZE = 4096;     // 4 KB (Conv1D W8, measured 2636)
    static constexpr int ONSET_MAX_WINDOW_FRAMES = 32; // Max 32 frames (512ms), expect W8
    alignas(16) uint8_t onsetArena_[ONSET_ARENA_SIZE];
    float onsetWindowBuffer_[ONSET_MAX_WINDOW_FRAMES * INPUT_MEL_BANDS];  // 3.3 KB max
    int onsetWindowFrames_ = 0;
    int onsetWindowFilled_ = 0;

    const tflite::Model* onsetModel_ = nullptr;
    tflite::MicroInterpreter* onsetInterpreter_ = nullptr;
    TfLiteTensor* onsetInput_ = nullptr;
    TfLiteTensor* onsetOutput_ = nullptr;
    int onsetOutputChannels_ = 1;
    int onsetOutputOffset_ = 0;
    float onsetLastBeat_ = 0.0f;
    bool onsetReady_ = false;
    int onsetInitError_ = 0;
    size_t onsetArenaUsed_ = 0;
    unsigned long onsetLastInferUs_ = 0;
    uint32_t onsetInferCount_ = 0;
    uint32_t onsetInvokeErrors_ = 0;

    // --- RhythmNN state ---

    static constexpr int RHYTHM_ARENA_SIZE = 20480;       // 20 KB (Conv1D+Pool W192, measured 15644)
    static constexpr int RHYTHM_MAX_WINDOW_FRAMES = 192;   // 192 frames (3.07s)
    alignas(16) uint8_t rhythmArena_[RHYTHM_ARENA_SIZE];
    float rhythmWindowBuffer_[RHYTHM_MAX_WINDOW_FRAMES * INPUT_MEL_BANDS];  // 19.5 KB
    int rhythmWindowFrames_ = 0;
    int rhythmWindowFilled_ = 0;

    const tflite::Model* rhythmModel_ = nullptr;
    tflite::MicroInterpreter* rhythmInterpreter_ = nullptr;
    TfLiteTensor* rhythmInput_ = nullptr;
    TfLiteTensor* rhythmOutput_ = nullptr;
    int rhythmOutputChannels_ = 1;  // Set to 2 by initModel if model has downbeat output
    int rhythmOutputOffset_ = 0;
    float rhythmLastBeat_ = 0.0f;
    float rhythmLastDownbeat_ = 0.0f;
    bool rhythmReady_ = false;
    int rhythmInitError_ = 0;
    size_t rhythmArenaUsed_ = 0;
    unsigned long rhythmLastInferUs_ = 0;
    uint32_t rhythmInferCount_ = 0;
    uint32_t rhythmInvokeErrors_ = 0;

    // --- Shared ---

    bool profileEnabled_ = false;
};
