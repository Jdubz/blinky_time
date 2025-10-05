#pragma once
#include "Effect.h"

/**
 * HueRotationEffect - Rotates the hue of all colors in the matrix
 *
 * This effect takes the generated pattern and shifts all colors by a
 * specified hue amount. Useful for creating color variations of the
 * same pattern (e.g., blue fire, green fire, etc.).
 *
 * Architecture: Inputs -> Generator -> HueRotationEffect (optional) -> Render -> LEDs
 */
class HueRotationEffect : public Effect {
private:
    float hueShift_;      // Hue shift amount (0.0 - 1.0)
    float rotationSpeed_; // How fast to auto-rotate (0.0 = static)
    unsigned long lastUpdateMs_;

    RGB rgbToHsv(const RGB& rgb) const;
    RGB hsvToRgb(float h, float s, float v) const;
    float normalizeHue(float hue) const;

public:
    HueRotationEffect(float initialHueShift = 0.0f, float rotationSpeed = 0.0f);
    virtual ~HueRotationEffect() = default;

    // Effect interface
    virtual void begin(int width, int height) override;
    virtual void apply(PixelMatrix* matrix) override;
    virtual void reset() override;
    virtual const char* getName() const override { return "HueRotation"; }

    // Configuration
    void setHueShift(float hueShift);
    void setRotationSpeed(float speed);

    float getHueShift() const { return hueShift_; }
    float getRotationSpeed() const { return rotationSpeed_; }
};
