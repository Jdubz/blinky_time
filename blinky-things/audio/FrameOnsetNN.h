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
//   - PLP source comparison (ACF evaluates NN onset alongside flux/bass)
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
    static constexpr int BAND_FLUX_CHANNELS = 3;   // Bass/mid/high HWR mel flux
    static constexpr int INPUT_MEL_PLUS_FLUX = INPUT_MEL_BANDS + BAND_FLUX_CHANNELS;  // 29
    static constexpr int MAX_INPUT_FEATURES = 52;  // Mel + delta (model auto-detects)

    // PCEN normalization mode: set before begin() to use PCEN instead of
    // log-compressed mel bands. Requires linear mel input from
    // SharedSpectralAnalysis::getLinearMelBands().
    void setPcenEnabled(bool enabled) { pcenEnabled_ = enabled; }

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
        // Detect input features per frame from model shape.
        // Supported: 26 (mel only), 29 (mel + 3 band-flux), 52 (mel + delta).
        int featuresPerFrame = input_->dims->data[input_->dims->size - 1];
        if (featuresPerFrame == INPUT_MEL_BANDS) {
            useDelta_ = false;
            useBandFlux_ = false;
        } else if (featuresPerFrame == INPUT_MEL_PLUS_FLUX) {
            useDelta_ = false;
            useBandFlux_ = true;
        } else if (featuresPerFrame == INPUT_MEL_BANDS * 2) {
            useDelta_ = true;
            useBandFlux_ = false;
        } else {
            initError_ = 5; return false;
        }
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
        memset(quantizedWindow_, 0, sizeof(quantizedWindow_));
        windowFilled_ = 0;
        windowWriteIdx_ = 0;

        // Cache quantization params for incremental quantization
        if (input_->type == kTfLiteInt8) {
            float scale = input_->params.scale;
            quantInvScale_ = (scale > 0.0f) ? (1.0f / scale) : 1.0f;
            quantZeroPoint_ = input_->params.zero_point;
        }

        ready_ = true;
        return true;
    }

    /**
     * Feed one frame of mel bands, run inference, return onset activation [0,1].
     *
     * When PCEN is enabled, also pass linearMelBands (pre-log linear energy).
     * The log-compressed melBands are still used for delta computation (matching
     * training pipeline behavior where delta is computed on log-compressed mels).
     */
    float infer(const float* melBands, const float* linearMelBands = nullptr) {
        if (!ready_) return 0.0f;

        // Apply PCEN if enabled, producing normalized mel values in [0,1]
        float pcenMel[INPUT_MEL_BANDS];
        const float* melInput = melBands;  // Default: log-compressed
        if (pcenEnabled_ && linearMelBands) {
            applyPcen(linearMelBands, pcenMel);
            melInput = pcenMel;
        }

        // Build the feature vector for this frame: mel bands + optional delta or band-flux
        float frameFeatures[MAX_INPUT_FEATURES];
        memcpy(frameFeatures, melInput, INPUT_MEL_BANDS * sizeof(float));
        if (useDelta_) {
            // Delta = mel[t] - mel[t-1]. First frame gets zero delta.
            for (int i = 0; i < INPUT_MEL_BANDS; i++) {
                frameFeatures[INPUT_MEL_BANDS + i] = melInput[i] - prevMel_[i];
            }
            memcpy(prevMel_, melInput, INPUT_MEL_BANDS * sizeof(float));
        } else if (useBandFlux_) {
            // 3-channel HWR band-flux: positive-only mel differences grouped by
            // frequency band, normalized by band count. Matches training pipeline
            // (audio.py::append_band_flux_features).
            computeBandFlux(melInput, &frameFeatures[INPUT_MEL_BANDS]);
            memcpy(prevMel_, melInput, INPUT_MEL_BANDS * sizeof(float));
        }

        // Circular window buffer: write new frame and quantize it incrementally.
        // Only the new frame (inputFeatures_ elements) is quantized per inference,
        // not the entire window. The quantized circular buffer is then copied to
        // the tensor input in chronological order via two memcpy's.
        int writeSlot = windowWriteIdx_;
        memcpy(&windowBuffer_[writeSlot * inputFeatures_],
               frameFeatures, inputFeatures_ * sizeof(float));

        // Incrementally quantize just this frame into the int8 circular buffer
        if (input_ && input_->type == kTfLiteInt8) {
            float invScale = quantInvScale_;
            int32_t zp = quantZeroPoint_;
            const float* src = frameFeatures;
            int8_t* dst = &quantizedWindow_[writeSlot * inputFeatures_];
            for (int j = 0; j < inputFeatures_; j++) {
                int32_t q = (int32_t)(src[j] * invScale + (src[j] >= 0 ? 0.5f : -0.5f)) + zp;
                dst[j] = (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
            }
        }

        windowWriteIdx_ = (windowWriteIdx_ + 1) % windowFrames_;
        if (windowFilled_ < windowFrames_) windowFilled_++;

        if (windowFilled_ < windowFrames_) return 0.0f;

        // Copy quantized circular buffer → tensor input in chronological order.
        // windowWriteIdx_ now points to the oldest frame.
        int frameBytes = inputFeatures_ * sizeof(int8_t);
        if (input_->type == kTfLiteInt8) {
            int8_t* input_data = input_->data.int8;
            int oldest = windowWriteIdx_;  // Oldest frame index
            int tailFrames = windowFrames_ - oldest;  // Frames from oldest to end of buffer
            // Copy tail (oldest → end of buffer)
            memcpy(input_data, &quantizedWindow_[oldest * inputFeatures_],
                   tailFrames * frameBytes);
            // Copy head (start of buffer → newest)
            if (oldest > 0) {
                memcpy(input_data + tailFrames * inputFeatures_,
                       quantizedWindow_, oldest * frameBytes);
            }
        } else {
            // Float model: copy in chronological order
            float* input_data = input_->data.f;
            int oldest = windowWriteIdx_;
            int tailFrames = windowFrames_ - oldest;
            memcpy(input_data, &windowBuffer_[oldest * inputFeatures_],
                   tailFrames * inputFeatures_ * sizeof(float));
            if (oldest > 0) {
                memcpy(input_data + tailFrames * inputFeatures_,
                       windowBuffer_, oldest * inputFeatures_ * sizeof(float));
            }
        }

        unsigned long t0 = micros();
        if (interpreter_->Invoke() != kTfLiteOk) {
            invokeErrors_++;
            return 0.0f;
        }
        lastInferUs_ = micros() - t0;
        inferCount_++;

        if (profileEnabled_ && (inferCount_ % 100 == 0)) {
            // Profiling output goes to Serial directly — this runs in the
            // hot audio path (~62.5 Hz) and cannot take a Print& parameter.
            // Profile data is developer-only and not routed to BLE/WiFi.
            Serial.print(F("[NNPROF] cnt="));
            Serial.print(inferCount_);
            Serial.print(F(" infer="));
            Serial.print(lastInferUs_);
            Serial.print(F("us arena="));
            Serial.print(arenaUsed_);
            Serial.print(F("/"));
            Serial.println(ARENA_SIZE);
        }

        // Extract onset output
        lastOnset_ = extractOutput(0);

        return lastOnset_;
    }

    // --- Status ---

    bool isReady() const { return ready_; }

    // --- Output accessors ---

    float getLastOnset() const { return lastOnset_; }

    // --- Profiling ---

    void setProfileEnabled(bool enabled) { profileEnabled_ = enabled; }
    bool isProfileEnabled() const { return profileEnabled_; }
    unsigned long getLastInferUs() const { return lastInferUs_; }
    uint32_t getInferCount() const { return inferCount_; }
    uint32_t getInvokeErrors() const { return invokeErrors_; }

    /** Print diagnostic info to the given output stream. */
    void printDiagnostics(Print& out) const {
        out.print(F("[NN] ready="));
        out.print(ready_ ? F("yes") : F("no"));
        if (ready_) {
            out.print(F(" arena="));
            out.print(arenaUsed_);
            out.print(F("/"));
            out.print(ARENA_SIZE);
            out.print(F(" window="));
            out.print(windowFrames_);
            out.print(F(" outputs="));
            out.print(outputChannels_);
            out.print(F(" infer="));
            out.print(lastInferUs_);
            out.print(F("us cnt="));
            out.print(inferCount_);
            if (invokeErrors_ > 0) {
                out.print(F(" ERRORS="));
                out.print(invokeErrors_);
            }
            out.print(F("\n  iscale="));
            out.print(input_->params.scale, 8);
            out.print(F(" izp="));
            out.print(input_->params.zero_point);
            out.print(F(" oscale="));
            out.print(output_->params.scale, 8);
            out.print(F(" ozp="));
            out.print(output_->params.zero_point);
            out.print(F("\n  onset="));
            out.print(lastOnset_, 4);
            if (input_->type == kTfLiteInt8 && windowFilled_ >= windowFrames_) {
                int lastFrameStart = (windowFrames_ - 1) * inputFeatures_;
                out.print(F("\n  last_mel=["));
                out.print(windowBuffer_[lastFrameStart], 4);
                out.print(F(","));
                out.print(windowBuffer_[lastFrameStart + 1], 4);
                out.print(F(",...,"));
                out.print(windowBuffer_[lastFrameStart + INPUT_MEL_BANDS - 1], 4);
                out.print(F("]"));
                if (useDelta_) {
                    out.print(F(" delta=["));
                    out.print(windowBuffer_[lastFrameStart + INPUT_MEL_BANDS], 4);
                    out.print(F(",...,"));
                    out.print(windowBuffer_[lastFrameStart + inputFeatures_ - 1], 4);
                    out.print(F("]"));
                }
            }
        } else {
            out.print(F(" error="));
            out.print(initError_);
        }
        out.println();
    }

private:
    // Op resolver — supports FC, Conv1D, and multi-output models:
    // FC: FullyConnected, Logistic, Quantize, Dequantize, Reshape
    // Conv1D: Conv2D, Pad, Reshape, ExpandDims, Logistic
    // Multi-output: Mul, StridedSlice, Concatenation, extra Quantize
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
        resolver_.AddMul();             // Kept for model compatibility (was sum_head)
        resolver_.AddStridedSlice();    // Kept for model compatibility (was multi-output split)
        resolver_.AddConcatenation();   // Kept for model compatibility (was multi-output join)
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

    static constexpr int ARENA_SIZE = 8192;            // 8 KB (Conv1D W16 arena varies by device: 3404-4564 observed)
    static constexpr int MAX_WINDOW_FRAMES = 64;      // Max 64 frames (1.024s) — increase if a wider model is used

    alignas(16) uint8_t arena_[ARENA_SIZE];
    float windowBuffer_[MAX_WINDOW_FRAMES * MAX_INPUT_FEATURES];  // 13 KB max (64 * 52 * 4)
    int8_t quantizedWindow_[MAX_WINDOW_FRAMES * MAX_INPUT_FEATURES];  // Pre-quantized circular buffer
    float prevMel_[INPUT_MEL_BANDS] = {0};      // Previous frame mel bands (for delta computation)

    // PCEN state (Lostanlen & Salamon 2019)
    bool pcenEnabled_ = false;
    bool pcenInitialized_ = false;
    float pcenSmooth_[INPUT_MEL_BANDS] = {0};  // IIR smoother state M[t] per band
    static constexpr float PCEN_S = 0.025f;    // IIR coefficient (~0.64s at 62.5 Hz)
    static constexpr float PCEN_DELTA = 2.0f;  // Stabilizer bias
    static constexpr float PCEN_EPS = 1e-6f;   // Numerical floor
    static constexpr float PCEN_NORM = 4.0f;   // Output normalization divisor

    /** Apply PCEN normalization to linear mel energy.
     *  P[t] = (E / (eps + M))^0.5 + delta)^0.5 - delta^0.5
     *  Simplified with alpha=1.0, r=0.5:
     *    P[t] = sqrt(E/(eps+M) + delta) - sqrt(delta) */
    void applyPcen(const float* linearMel, float* out) {
        const float sqrtDelta = sqrtf(PCEN_DELTA);
        if (!pcenInitialized_) {
            memcpy(pcenSmooth_, linearMel, INPUT_MEL_BANDS * sizeof(float));
            pcenInitialized_ = true;
        }
        for (int i = 0; i < INPUT_MEL_BANDS; i++) {
            float E = linearMel[i];
            pcenSmooth_[i] = PCEN_S * E + (1.0f - PCEN_S) * pcenSmooth_[i];
            float agc = E / (PCEN_EPS + pcenSmooth_[i]);
            float val = sqrtf(agc + PCEN_DELTA) - sqrtDelta;
            val /= PCEN_NORM;
            out[i] = (val < 0.0f) ? 0.0f : ((val > 1.0f) ? 1.0f : val);
        }
    }

    /** Compute 3-channel HWR band-flux from mel frame difference.
     *  Bass (0-5): kicks. Mid (6-13): vocals/pads. High (14-25): snares/hi-hats.
     *  Each band = sum(max(0, mel[t]-mel[t-1])) / band_count. */
    void computeBandFlux(const float* mel, float* out) {
        float bass = 0.0f, mid = 0.0f, high = 0.0f;
        for (int i = 0; i < INPUT_MEL_BANDS; i++) {
            float d = mel[i] - prevMel_[i];
            if (d > 0.0f) {
                if (i < 6)        bass += d;
                else if (i < 14)  mid += d;
                else              high += d;
            }
        }
        out[0] = bass / 6.0f;
        out[1] = mid / 8.0f;
        out[2] = high / 12.0f;
    }

    int windowFrames_ = 0;
    int windowFilled_ = 0;
    int windowWriteIdx_ = 0;                    // Circular write position in windowBuffer_
    int inputFeatures_ = INPUT_MEL_BANDS;       // 26, 29, or 52, auto-detected from model
    bool useDelta_ = false;                      // True when model expects 52-dim input (mel + delta)
    bool useBandFlux_ = false;                   // True when model expects 29-dim input (mel + 3 band-flux)
    float quantInvScale_ = 1.0f;                // Cached 1.0/input_scale for quantization
    int32_t quantZeroPoint_ = 0;                // Cached input zero point

    const tflite::Model* model_ = nullptr;
    tflite::MicroInterpreter* interpreter_ = nullptr;
    TfLiteTensor* input_ = nullptr;
    TfLiteTensor* output_ = nullptr;
    int outputChannels_ = 1;
    int outputOffset_ = 0;
    float lastOnset_ = 0.0f;
    bool ready_ = false;
    int initError_ = 0;
    size_t arenaUsed_ = 0;
    unsigned long lastInferUs_ = 0;
    uint32_t inferCount_ = 0;
    uint32_t invokeErrors_ = 0;
    bool profileEnabled_ = false;
};
