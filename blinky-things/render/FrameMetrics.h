#pragma once

#include "../types/PixelMatrix.h"
#include <stdint.h>
#include <math.h>
#include <new>           // std::nothrow

/**
 * FrameMetrics — per-frame visual measurements over the PixelMatrix.
 *
 * Computed AFTER the generator + effect chain, BEFORE the LED driver.
 * Output is what the visualizer is producing (not what the LEDs are
 * actually emitting; that requires a camera).
 *
 * Hooked into RenderPipeline::render() between effect.apply() and
 * renderer_->render(). Streamed via SerialConsole as the "v" block in
 * the existing audio stream JSON so an agent can correlate audio
 * control signals with the generator's response in real time.
 *
 * All computations are single-pass over the pixel buffer. Cost on
 * nRF52840 @ 64 MHz with 240 LEDs is ~50 us/frame (<1% CPU at 80 fps).
 *
 * Metrics produced per frame:
 *   avgL       Average luminance (Rec. 601: 0.299R + 0.587G + 0.114B), 0-255
 *   maxL       Peak luminance in this frame, 0-255
 *   rmsContrast  Std-dev of luminance over the frame, 0-255 (within-frame contrast)
 *   activity   Sum of |frame_t - frame_{t-1}| / numPixels / 255, 0-1
 *   centroidX  Luminance-weighted centroid X (0 to width-1), 0 if frame is dark
 *   centroidY  Luminance-weighted centroid Y (0 to height-1)
 *   satMean    Average HSV saturation, 0-1
 *
 * The previous-frame buffer for `activity` is a uint8_t per pixel
 * (only luminance kept), so memory cost is `numPixels` bytes. For the
 * largest device in the fleet (BucketTotem at 128 LEDs) that's 128 B.
 */
class FrameMetrics {
public:
    FrameMetrics()
        : avgL_(0), maxL_(0), rmsContrast_(0), activity_(0),
          centroidX_(0), centroidY_(0), satMean_(0),
          prevLum_(nullptr), prevLumCapacity_(0), prevValid_(false) {}

    ~FrameMetrics() {
        if (prevLum_) delete[] prevLum_;
    }

    // Reset state. Call when geometry changes or on first use.
    void reset(int numPixels) {
        if (numPixels > prevLumCapacity_) {
            if (prevLum_) delete[] prevLum_;
            prevLum_ = new(std::nothrow) uint8_t[numPixels];
            prevLumCapacity_ = prevLum_ ? numPixels : 0;
        }
        if (prevLum_) {
            for (int i = 0; i < prevLumCapacity_; i++) prevLum_[i] = 0;
        }
        prevValid_ = false;
        avgL_ = maxL_ = rmsContrast_ = activity_ = 0;
        centroidX_ = centroidY_ = satMean_ = 0;
    }

    // Compute metrics for this frame. Called once per render() pass.
    // Safe to call before reset() — does nothing if no prev buffer.
    void processFrame(const PixelMatrix& matrix) {
        const int w = matrix.getWidth();
        const int h = matrix.getHeight();
        const int n = w * h;
        if (n <= 0) return;

        // Lazy-allocate on first call if reset() wasn't called yet.
        if (!prevLum_ || prevLumCapacity_ < n) reset(n);
        if (!prevLum_) return;  // alloc failed — metrics stay zero

        uint32_t sumL = 0;
        uint32_t sumLsq = 0;       // for variance (RMS contrast)
        uint8_t  maxL = 0;
        uint32_t sumDeltaL = 0;    // for activity
        uint32_t sumSat = 0;       // saturation, 0-255 per pixel
        uint32_t sumLx = 0;        // luminance-weighted x
        uint32_t sumLy = 0;        // luminance-weighted y

        // Single pass over the matrix in row-major order. Mirrors what
        // the LED driver consumes (so centroid coords match the visual
        // layout, not some internal storage order).
        int i = 0;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const RGB& p = matrix.getPixel(x, y);
                // Rec. 601 luminance with integer weights (sum=1000):
                //   299*R + 587*G + 114*B, scaled back by 1000.
                uint32_t lum1000 = 299u * p.r + 587u * p.g + 114u * p.b;
                uint8_t  L = (uint8_t)(lum1000 / 1000u);

                sumL  += L;
                sumLsq += (uint32_t)L * L;
                if (L > maxL) maxL = L;
                sumLx += (uint32_t)L * (uint32_t)x;
                sumLy += (uint32_t)L * (uint32_t)y;

                // HSV saturation = (max - min) / max, scaled to 0-255.
                uint8_t mx = p.r > p.g ? (p.r > p.b ? p.r : p.b) : (p.g > p.b ? p.g : p.b);
                uint8_t mn = p.r < p.g ? (p.r < p.b ? p.r : p.b) : (p.g < p.b ? p.g : p.b);
                if (mx > 0) sumSat += ((uint32_t)(mx - mn) * 255u) / mx;

                if (prevValid_) {
                    int d = (int)L - (int)prevLum_[i];
                    sumDeltaL += (uint32_t)(d < 0 ? -d : d);
                }
                prevLum_[i] = L;
                i++;
            }
        }

        const float invN = 1.0f / (float)n;
        avgL_       = (float)sumL * invN;
        maxL_       = (float)maxL;
        // RMS contrast = sqrt(E[L^2] - E[L]^2)
        float meanLsq = (float)sumLsq * invN;
        float var = meanLsq - avgL_ * avgL_;
        rmsContrast_ = var > 0.0f ? sqrtf(var) : 0.0f;
        // Activity normalised to 0-1 (max possible per-pixel delta = 255).
        activity_   = prevValid_ ? ((float)sumDeltaL * invN) / 255.0f : 0.0f;
        satMean_    = (float)sumSat * invN / 255.0f;
        // Centroid: undefined when frame is dark; report 0,0 so consumers
        // can gate on (avgL_ > some_threshold) rather than NaN-checking.
        if (sumL > 0) {
            centroidX_ = (float)sumLx / (float)sumL;
            centroidY_ = (float)sumLy / (float)sumL;
        } else {
            centroidX_ = 0.0f;
            centroidY_ = 0.0f;
        }

        prevValid_ = true;
    }

    // Getters — last computed frame.
    float getAvgL()       const { return avgL_; }
    float getMaxL()       const { return maxL_; }
    float getRmsContrast()const { return rmsContrast_; }
    float getActivity()   const { return activity_; }
    float getCentroidX()  const { return centroidX_; }
    float getCentroidY()  const { return centroidY_; }
    float getSatMean()    const { return satMean_; }

private:
    float avgL_;
    float maxL_;
    float rmsContrast_;
    float activity_;
    float centroidX_;
    float centroidY_;
    float satMean_;

    uint8_t* prevLum_;          // Previous-frame luminance buffer
    int      prevLumCapacity_;
    bool     prevValid_;

    // Prevent copying (manages a heap buffer)
    FrameMetrics(const FrameMetrics&) = delete;
    FrameMetrics& operator=(const FrameMetrics&) = delete;
};
