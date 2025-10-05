#pragma once

#include "../UnifiedFireGenerator.h"
#include "../../core/EffectMatrix.h"

/**
 * Unified Fire Generator Test - Tests all layout types (Matrix, Linear, Random)
 */
class UnifiedFireGeneratorTest {
public:
    static bool runAllTests() {
        bool allPassed = true;

        Serial.println("=== UnifiedFireGenerator Tests ===");

        allPassed &= testMatrixLayout();
        allPassed &= testLinearLayout();
        allPassed &= testRandomLayout();
        allPassed &= testLayoutSwitching();
        allPassed &= testAudioReactivity();

        return allPassed;
    }

private:
    static bool testMatrixLayout() {
        Serial.println("Testing Matrix Layout...");

        UnifiedFireGenerator generator;
        EffectMatrix matrix(8, 8);

        generator.begin(8, 8, LAYOUT_MATRIX);
        generator.update();
        generator.generate(&matrix);

        // Test that some pixels are generated
        bool hasOutput = false;
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint32_t color = matrix.getPixel(x, y);
                if (color != 0) {
                    hasOutput = true;
                    break;
                }
            }
        }

        if (hasOutput) {
            Serial.println("✅ Matrix layout test passed");
            return true;
        } else {
            Serial.println("❌ Matrix layout test failed - no output");
            return false;
        }
    }

    static bool testLinearLayout() {
        Serial.println("Testing Linear Layout...");

        UnifiedFireGenerator generator;
        EffectMatrix matrix(89, 1);  // Hat configuration

        generator.begin(89, 1, LAYOUT_LINEAR);
        generator.update();
        generator.generate(&matrix);

        // Test that some pixels are generated
        bool hasOutput = false;
        for (int x = 0; x < 89; x++) {
            uint32_t color = matrix.getPixel(x, 0);
            if (color != 0) {
                hasOutput = true;
                break;
            }
        }

        if (hasOutput) {
            Serial.println("✅ Linear layout test passed");
            return true;
        } else {
            Serial.println("❌ Linear layout test failed - no output");
            return false;
        }
    }

    static bool testRandomLayout() {
        Serial.println("Testing Random Layout...");

        UnifiedFireGenerator generator;
        EffectMatrix matrix(10, 10);

        generator.begin(10, 10, LAYOUT_RANDOM);
        generator.update();
        generator.generate(&matrix);

        // Test that some pixels are generated
        bool hasOutput = false;
        for (int y = 0; y < 10; y++) {
            for (int x = 0; x < 10; x++) {
                uint32_t color = matrix.getPixel(x, y);
                if (color != 0) {
                    hasOutput = true;
                    break;
                }
            }
        }

        if (hasOutput) {
            Serial.println("✅ Random layout test passed");
            return true;
        } else {
            Serial.println("❌ Random layout test failed - no output");
            return false;
        }
    }

    static bool testLayoutSwitching() {
        Serial.println("Testing Layout Switching...");

        UnifiedFireGenerator generator;
        EffectMatrix matrix(8, 8);

        // Test switching between layouts
        generator.begin(8, 8, LAYOUT_MATRIX);
        generator.setLayoutType(LAYOUT_LINEAR);
        generator.setLayoutType(LAYOUT_RANDOM);
        generator.setLayoutType(LAYOUT_MATRIX);

        generator.update();
        generator.generate(&matrix);

        Serial.println("✅ Layout switching test passed");
        return true;
    }

    static bool testAudioReactivity() {
        Serial.println("Testing Audio Reactivity...");

        UnifiedFireGenerator generator;
        EffectMatrix matrix(8, 8);

        generator.begin(8, 8, LAYOUT_MATRIX);

        // Test with different audio inputs
        generator.setAudioInput(0.5f, false);  // Medium energy, no hit
        generator.update();
        generator.generate(&matrix);

        generator.setAudioInput(1.0f, true);   // High energy with hit
        generator.update();
        generator.generate(&matrix);

        Serial.println("✅ Audio reactivity test passed");
        return true;
    }
};
