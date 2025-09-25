#include "HueRotationEffect.h"
#include <Arduino.h>

HueRotationEffect::HueRotationEffect(float initialHueShift, float rotationSpeed) 
    : hueShift_(initialHueShift), rotationSpeed_(rotationSpeed), lastUpdateMs_(0) {
}

void HueRotationEffect::begin(int width, int height) {
    // Nothing special needed for initialization
    lastUpdateMs_ = millis();
}

void HueRotationEffect::apply(EffectMatrix* matrix) {
    if (!matrix) return;
    
    // Update hue shift if auto-rotating
    if (rotationSpeed_ != 0.0f) {
        unsigned long currentMs = millis();
        if (lastUpdateMs_ != 0) {
            float deltaSeconds = (currentMs - lastUpdateMs_) / 1000.0f;
            hueShift_ += rotationSpeed_ * deltaSeconds;
            hueShift_ = normalizeHue(hueShift_);
        }
        lastUpdateMs_ = currentMs;
    }
    
    // Apply hue shift to all pixels in the matrix
    int width = matrix->getWidth();
    int height = matrix->getHeight();
    
    for (int x = 0; x < width; x++) {
        for (int y = 0; y < height; y++) {
            RGB originalColor = matrix->getPixel(x, y);
            
            // Skip black pixels (no color to shift)
            if (originalColor.r == 0 && originalColor.g == 0 && originalColor.b == 0) {
                continue;
            }
            
            // Convert RGB to HSV
            RGB hsv = rgbToHsv(originalColor);
            float h = hsv.r / 255.0f; // H stored in r channel
            float s = hsv.g / 255.0f; // S stored in g channel  
            float v = hsv.b / 255.0f; // V stored in b channel
            
            // Apply hue shift
            h = normalizeHue(h + hueShift_);
            
            // Convert back to RGB
            RGB newColor = hsvToRgb(h, s, v);
            matrix->setPixel(x, y, newColor);
        }
    }
}

void HueRotationEffect::setHueShift(float hueShift) {
    hueShift_ = normalizeHue(hueShift);
}

void HueRotationEffect::setRotationSpeed(float speed) {
    rotationSpeed_ = speed;
}

float HueRotationEffect::normalizeHue(float hue) const {
    while (hue < 0.0f) hue += 1.0f;
    while (hue >= 1.0f) hue -= 1.0f;
    return hue;
}

RGB HueRotationEffect::rgbToHsv(const RGB& rgb) const {
    float r = rgb.r / 255.0f;
    float g = rgb.g / 255.0f;
    float b = rgb.b / 255.0f;
    
    float max_val = max(max(r, g), b);
    float min_val = min(min(r, g), b);
    float delta = max_val - min_val;
    
    float h = 0.0f;
    float s = (max_val == 0.0f) ? 0.0f : delta / max_val;
    float v = max_val;
    
    if (delta != 0.0f) {
        if (max_val == r) {
            h = (g - b) / delta;
            if (h < 0.0f) h += 6.0f;
        } else if (max_val == g) {
            h = 2.0f + (b - r) / delta;
        } else { // max_val == b
            h = 4.0f + (r - g) / delta;
        }
        h /= 6.0f;
    }
    
    return RGB{
        (uint8_t)(h * 255),
        (uint8_t)(s * 255),
        (uint8_t)(v * 255)
    };
}

RGB HueRotationEffect::hsvToRgb(float h, float s, float v) const {
    if (s == 0.0f) {
        // Grayscale
        uint8_t gray = (uint8_t)(v * 255);
        return RGB{gray, gray, gray};
    }
    
    h *= 6.0f;
    int i = (int)h;
    float f = h - i;
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    
    float r, g, b;
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
        default: r = g = b = 0.0f; break;
    }
    
    return RGB{
        (uint8_t)(r * 255),
        (uint8_t)(g * 255),
        (uint8_t)(b * 255)
    };
}