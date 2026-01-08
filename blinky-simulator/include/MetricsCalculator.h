#pragma once
/**
 * MetricsCalculator.h - Compute visual metrics for agent optimization
 *
 * Analyzes rendered frames to produce quantitative feedback:
 * - Brightness distribution (mean, variance, dynamic range)
 * - Activity level (pixel changes between frames)
 * - Color utilization (hue spread, saturation)
 */

#include <vector>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <string>
#include <cstdint>

struct VisualMetrics {
    // Brightness metrics (0-255 scale)
    float avgBrightness = 0;
    float minBrightness = 255;
    float maxBrightness = 0;
    float brightnessVariance = 0;
    float dynamicRange = 0;  // max - min across all frames

    // Activity metrics (0-1 scale)
    float avgActivity = 0;        // Average pixel change between frames
    float peakActivity = 0;       // Maximum activity spike
    float activityVariance = 0;   // How variable the activity is

    // Color metrics
    float avgSaturation = 0;      // Average color saturation (0-1)
    float hueSpread = 0;          // Hue diversity (0-1, 1 = full rainbow)
    float colorfulness = 0;       // Combined saturation * hue_spread

    // Frame statistics
    int totalFrames = 0;
    int litPixelPercent = 0;      // Average % of pixels that are lit (>10 brightness)
    int deadFrames = 0;           // Frames with <5% lit pixels
};

class MetricsCalculator {
public:
    void reset() {
        frameBrightnesses_.clear();
        frameActivities_.clear();
        frameSaturations_.clear();
        hueHistogram_.clear();
        hueHistogram_.resize(12, 0);  // 12 hue buckets (30 degrees each)
        prevFrame_.clear();
        totalLitPixels_ = 0;
        totalPixels_ = 0;
        deadFrameCount_ = 0;
        frameCount_ = 0;
    }

    /**
     * Process a frame of RGB data
     * @param buffer RGB pixel data (3 bytes per pixel)
     * @param pixelCount Number of pixels
     */
    void processFrame(const uint8_t* buffer, int pixelCount) {
        float frameBrightness = 0;
        float frameSaturation = 0;
        float frameActivity = 0;
        int litPixels = 0;

        std::vector<uint8_t> currentFrame(buffer, buffer + pixelCount * 3);

        for (int i = 0; i < pixelCount; i++) {
            uint8_t r = buffer[i * 3];
            uint8_t g = buffer[i * 3 + 1];
            uint8_t b = buffer[i * 3 + 2];

            // Brightness (luminance approximation)
            float brightness = 0.299f * r + 0.587f * g + 0.114f * b;
            frameBrightness += brightness;

            if (brightness < minBrightness_) minBrightness_ = brightness;
            if (brightness > maxBrightness_) maxBrightness_ = brightness;

            // Track lit pixels
            if (brightness > 10) litPixels++;

            // Saturation and hue
            float maxC = std::max({r, g, b}) / 255.0f;
            float minC = std::min({r, g, b}) / 255.0f;
            float saturation = (maxC > 0) ? (maxC - minC) / maxC : 0;
            frameSaturation += saturation;

            // Hue bucket for color diversity
            if (saturation > 0.1f && brightness > 10) {
                float hue = computeHue(r, g, b);
                int bucket = (int)(hue / 30.0f) % 12;
                hueHistogram_[bucket]++;
            }

            // Activity (pixel change from previous frame)
            if (!prevFrame_.empty()) {
                int dr = abs((int)r - (int)prevFrame_[i * 3]);
                int dg = abs((int)g - (int)prevFrame_[i * 3 + 1]);
                int db = abs((int)b - (int)prevFrame_[i * 3 + 2]);
                frameActivity += (dr + dg + db) / 765.0f;  // Normalize to 0-1
            }
        }

        frameBrightness /= pixelCount;
        frameSaturation /= pixelCount;
        frameActivity /= pixelCount;

        frameBrightnesses_.push_back(frameBrightness);
        frameSaturations_.push_back(frameSaturation);
        if (!prevFrame_.empty()) {
            frameActivities_.push_back(frameActivity);
        }

        totalLitPixels_ += litPixels;
        totalPixels_ += pixelCount;
        if (litPixels < pixelCount * 0.05f) deadFrameCount_++;

        prevFrame_ = currentFrame;
        frameCount_++;
    }

    /**
     * Compute final metrics after all frames processed
     */
    VisualMetrics compute() {
        VisualMetrics m;
        m.totalFrames = frameCount_;

        if (frameCount_ == 0) return m;

        // Brightness stats
        m.avgBrightness = mean(frameBrightnesses_);
        m.brightnessVariance = variance(frameBrightnesses_, m.avgBrightness);
        m.minBrightness = minBrightness_;
        m.maxBrightness = maxBrightness_;
        m.dynamicRange = maxBrightness_ - minBrightness_;

        // Activity stats
        if (!frameActivities_.empty()) {
            m.avgActivity = mean(frameActivities_);
            m.peakActivity = *std::max_element(frameActivities_.begin(), frameActivities_.end());
            m.activityVariance = variance(frameActivities_, m.avgActivity);
        }

        // Color stats
        m.avgSaturation = mean(frameSaturations_);
        m.hueSpread = computeHueSpread();
        m.colorfulness = m.avgSaturation * m.hueSpread;

        // Frame stats
        m.litPixelPercent = (totalPixels_ > 0) ? (100 * totalLitPixels_ / totalPixels_) : 0;
        m.deadFrames = deadFrameCount_;

        return m;
    }

    /**
     * Write metrics to JSON file
     */
    static void writeJson(const std::string& path, const VisualMetrics& m) {
        std::ofstream file(path);
        file << "{\n";
        file << "  \"frames\": " << m.totalFrames << ",\n";
        file << "  \"brightness\": {\n";
        file << "    \"avg\": " << m.avgBrightness << ",\n";
        file << "    \"min\": " << m.minBrightness << ",\n";
        file << "    \"max\": " << m.maxBrightness << ",\n";
        file << "    \"variance\": " << m.brightnessVariance << ",\n";
        file << "    \"dynamicRange\": " << m.dynamicRange << "\n";
        file << "  },\n";
        file << "  \"activity\": {\n";
        file << "    \"avg\": " << m.avgActivity << ",\n";
        file << "    \"peak\": " << m.peakActivity << ",\n";
        file << "    \"variance\": " << m.activityVariance << "\n";
        file << "  },\n";
        file << "  \"color\": {\n";
        file << "    \"saturation\": " << m.avgSaturation << ",\n";
        file << "    \"hueSpread\": " << m.hueSpread << ",\n";
        file << "    \"colorfulness\": " << m.colorfulness << "\n";
        file << "  },\n";
        file << "  \"litPixelPercent\": " << m.litPixelPercent << ",\n";
        file << "  \"deadFrames\": " << m.deadFrames << "\n";
        file << "}\n";
        file.close();
    }

private:
    std::vector<float> frameBrightnesses_;
    std::vector<float> frameActivities_;
    std::vector<float> frameSaturations_;
    std::vector<int> hueHistogram_;
    std::vector<uint8_t> prevFrame_;

    float minBrightness_ = 255;
    float maxBrightness_ = 0;
    int totalLitPixels_ = 0;
    int totalPixels_ = 0;
    int deadFrameCount_ = 0;
    int frameCount_ = 0;

    static float mean(const std::vector<float>& v) {
        if (v.empty()) return 0;
        float sum = 0;
        for (float x : v) sum += x;
        return sum / v.size();
    }

    static float variance(const std::vector<float>& v, float mean) {
        if (v.size() < 2) return 0;
        float sum = 0;
        for (float x : v) {
            float d = x - mean;
            sum += d * d;
        }
        return sum / (v.size() - 1);
    }

    static float computeHue(uint8_t r, uint8_t g, uint8_t b) {
        float rf = r / 255.0f;
        float gf = g / 255.0f;
        float bf = b / 255.0f;
        float maxC = std::max({rf, gf, bf});
        float minC = std::min({rf, gf, bf});
        float delta = maxC - minC;

        if (delta < 0.001f) return 0;

        float hue;
        if (maxC == rf) {
            hue = 60.0f * fmod((gf - bf) / delta, 6.0f);
        } else if (maxC == gf) {
            hue = 60.0f * ((bf - rf) / delta + 2.0f);
        } else {
            hue = 60.0f * ((rf - gf) / delta + 4.0f);
        }

        if (hue < 0) hue += 360;
        return hue;
    }

    float computeHueSpread() {
        // Count non-empty hue buckets
        int nonEmpty = 0;
        for (int count : hueHistogram_) {
            if (count > 0) nonEmpty++;
        }
        return nonEmpty / 12.0f;  // 0-1 scale
    }
};
