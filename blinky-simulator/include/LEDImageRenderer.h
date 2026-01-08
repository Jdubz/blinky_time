#pragma once
/**
 * LEDImageRenderer - Renders LED strip state to an RGBA image buffer
 *
 * Converts MockLedStrip pixel data to a visual representation suitable
 * for GIF/PNG export. Supports multiple layout styles.
 */

#include <cstdint>
#include <cstring>
#include <vector>
#include <cmath>

// Include MockLedStrip from blinky-things
#include "../../blinky-things/hal/mock/MockLedStrip.h"

enum class LEDLayoutStyle {
    GRID,       // 2D grid layout (for matrix devices)
    STRIP,      // Horizontal strip (for string devices)
    CIRCLE,     // Circular arrangement
    VERTICAL    // Vertical strip
};

struct LEDRenderConfig {
    int ledWidth = 4;           // Grid width (columns)
    int ledHeight = 15;         // Grid height (rows)
    int ledSize = 20;           // LED circle/square size in pixels
    int ledSpacing = 4;         // Space between LEDs
    int padding = 10;           // Image border padding
    LEDLayoutStyle style = LEDLayoutStyle::GRID;
    bool drawGlow = true;       // Add glow effect around LEDs
    uint8_t backgroundColor[3] = {20, 20, 25}; // Dark background
};

class LEDImageRenderer {
private:
    LEDRenderConfig config_;
    std::vector<uint8_t> buffer_;  // RGBA buffer
    int imageWidth_ = 0;
    int imageHeight_ = 0;

    // Helper: blend pixel with alpha
    void blendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        if (x < 0 || x >= imageWidth_ || y < 0 || y >= imageHeight_) return;

        int idx = (y * imageWidth_ + x) * 4;
        float alpha = a / 255.0f;
        float invAlpha = 1.0f - alpha;

        buffer_[idx + 0] = static_cast<uint8_t>(r * alpha + buffer_[idx + 0] * invAlpha);
        buffer_[idx + 1] = static_cast<uint8_t>(g * alpha + buffer_[idx + 1] * invAlpha);
        buffer_[idx + 2] = static_cast<uint8_t>(b * alpha + buffer_[idx + 2] * invAlpha);
        buffer_[idx + 3] = 255;
    }

    // Helper: draw filled circle
    void drawFilledCircle(int cx, int cy, int radius, uint8_t r, uint8_t g, uint8_t b) {
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (dist <= radius) {
                    // Anti-aliased edge
                    float edgeDist = radius - dist;
                    uint8_t alpha = edgeDist < 1.0f ? static_cast<uint8_t>(edgeDist * 255) : 255;
                    blendPixel(cx + dx, cy + dy, r, g, b, alpha);
                }
            }
        }
    }

    // Helper: draw glow around LED
    void drawGlow(int cx, int cy, int radius, uint8_t r, uint8_t g, uint8_t b) {
        int glowRadius = radius * 2;
        for (int dy = -glowRadius; dy <= glowRadius; dy++) {
            for (int dx = -glowRadius; dx <= glowRadius; dx++) {
                float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (dist > radius && dist <= glowRadius) {
                    // Glow intensity falls off with distance
                    float intensity = 1.0f - (dist - radius) / (glowRadius - radius);
                    intensity = intensity * intensity; // Quadratic falloff
                    uint8_t alpha = static_cast<uint8_t>(intensity * 80); // Max 80 alpha for glow
                    blendPixel(cx + dx, cy + dy, r, g, b, alpha);
                }
            }
        }
    }

    // Get LED center position based on layout style
    void getLEDPosition(int ledIndex, int& cx, int& cy) {
        int cellSize = config_.ledSize + config_.ledSpacing;

        switch (config_.style) {
            case LEDLayoutStyle::GRID: {
                // For VERTICAL orientation (like tube light), columns are wired in zigzag
                int col = ledIndex / config_.ledHeight;
                int rowInCol = ledIndex % config_.ledHeight;
                // Odd columns are reversed
                int row = (col % 2 == 0) ? rowInCol : (config_.ledHeight - 1 - rowInCol);

                cx = config_.padding + col * cellSize + config_.ledSize / 2;
                cy = config_.padding + row * cellSize + config_.ledSize / 2;
                break;
            }

            case LEDLayoutStyle::STRIP: {
                cx = config_.padding + ledIndex * cellSize + config_.ledSize / 2;
                cy = config_.padding + config_.ledSize / 2;
                break;
            }

            case LEDLayoutStyle::VERTICAL: {
                cx = config_.padding + config_.ledSize / 2;
                cy = config_.padding + ledIndex * cellSize + config_.ledSize / 2;
                break;
            }

            case LEDLayoutStyle::CIRCLE: {
                int totalLeds = config_.ledWidth * config_.ledHeight;
                float angle = (2.0f * 3.14159265f * ledIndex) / totalLeds;
                float radius = std::min(imageWidth_, imageHeight_) / 2.0f - config_.padding - config_.ledSize;
                cx = imageWidth_ / 2 + static_cast<int>(radius * std::cos(angle));
                cy = imageHeight_ / 2 + static_cast<int>(radius * std::sin(angle));
                break;
            }
        }
    }

public:
    LEDImageRenderer() = default;

    void configure(const LEDRenderConfig& config) {
        config_ = config;

        int cellSize = config_.ledSize + config_.ledSpacing;

        // Calculate image dimensions based on layout style
        switch (config_.style) {
            case LEDLayoutStyle::GRID:
                imageWidth_ = config_.padding * 2 + config_.ledWidth * cellSize - config_.ledSpacing;
                imageHeight_ = config_.padding * 2 + config_.ledHeight * cellSize - config_.ledSpacing;
                break;

            case LEDLayoutStyle::STRIP:
                imageWidth_ = config_.padding * 2 + config_.ledWidth * config_.ledHeight * cellSize - config_.ledSpacing;
                imageHeight_ = config_.padding * 2 + config_.ledSize;
                break;

            case LEDLayoutStyle::VERTICAL:
                imageWidth_ = config_.padding * 2 + config_.ledSize;
                imageHeight_ = config_.padding * 2 + config_.ledWidth * config_.ledHeight * cellSize - config_.ledSpacing;
                break;

            case LEDLayoutStyle::CIRCLE: {
                int totalLeds = config_.ledWidth * config_.ledHeight;
                int diameter = totalLeds * cellSize / 3 + config_.padding * 2 + config_.ledSize * 2;
                imageWidth_ = diameter;
                imageHeight_ = diameter;
                break;
            }
        }

        // Allocate buffer
        buffer_.resize(imageWidth_ * imageHeight_ * 4);
    }

    // Render LED strip state to image buffer
    void render(const MockLedStrip& leds) {
        // Clear to background color
        for (int i = 0; i < imageWidth_ * imageHeight_; i++) {
            buffer_[i * 4 + 0] = config_.backgroundColor[0];
            buffer_[i * 4 + 1] = config_.backgroundColor[1];
            buffer_[i * 4 + 2] = config_.backgroundColor[2];
            buffer_[i * 4 + 3] = 255;
        }

        // Draw each LED
        int numLeds = leds.numPixels();
        int radius = config_.ledSize / 2;

        for (int i = 0; i < numLeds; i++) {
            int cx, cy;
            getLEDPosition(i, cx, cy);

            uint32_t color = leds.getPixelColor(i);
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;

            // Draw glow first (if enabled and LED is lit)
            if (config_.drawGlow && (r > 0 || g > 0 || b > 0)) {
                drawGlow(cx, cy, radius, r, g, b);
            }

            // Draw LED
            drawFilledCircle(cx, cy, radius, r, g, b);

            // Draw LED outline (dark ring) for visibility even when off
            if (r == 0 && g == 0 && b == 0) {
                // Draw dim outline for off LEDs
                for (int angle = 0; angle < 360; angle += 10) {
                    float rad = angle * 3.14159265f / 180.0f;
                    int ox = cx + static_cast<int>(radius * std::cos(rad));
                    int oy = cy + static_cast<int>(radius * std::sin(rad));
                    blendPixel(ox, oy, 40, 40, 45, 255);
                }
            }
        }
    }

    // Get image dimensions
    int getWidth() const { return imageWidth_; }
    int getHeight() const { return imageHeight_; }

    // Get raw RGBA buffer (for GIF/PNG encoding)
    const uint8_t* getBuffer() const { return buffer_.data(); }
    uint8_t* getBuffer() { return buffer_.data(); }
    size_t getBufferSize() const { return buffer_.size(); }

    // Get RGB buffer (no alpha, for simpler encoders)
    std::vector<uint8_t> getRGBBuffer() const {
        std::vector<uint8_t> rgb(imageWidth_ * imageHeight_ * 3);
        for (int i = 0; i < imageWidth_ * imageHeight_; i++) {
            rgb[i * 3 + 0] = buffer_[i * 4 + 0];
            rgb[i * 3 + 1] = buffer_[i * 4 + 1];
            rgb[i * 3 + 2] = buffer_[i * 4 + 2];
        }
        return rgb;
    }
};
