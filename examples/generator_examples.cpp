/**
 * Generator Examples - How to use Fire, Water, and Lightning generators
 *
 * This example shows how to create and use different generator instances
 * for various visual effects. All generators work with any device layout.
 */

#include "generators/Generator.h"
#include "generators/Fire.h"
#include "generators/Water.h"
#include "generators/Lightning.h"

// Example: Create generator instances
Fire fireGenerator;
Water waterGenerator;
Lightning lightningGenerator;

// Example: Generator switching system
Generator* generators[] = {
    &fireGenerator,
    &waterGenerator,
    &lightningGenerator
};

const char* generatorNames[] = {
    "Fire",
    "Water",
    "Lightning"
};

int currentGeneratorIndex = 0;
const int numGenerators = 3;

void setupGenerators(const DeviceConfig& config) {
    // Initialize all generators with device configuration
    for (int i = 0; i < numGenerators; i++) {
        if (!generators[i]->begin(config)) {
            Serial.print(F("ERROR: Failed to initialize "));
            Serial.println(generatorNames[i]);
        } else {
            Serial.print(F("✅ "));
            Serial.print(generatorNames[i]);
            Serial.println(F(" generator ready"));
        }
    }
}

void switchToNextGenerator() {
    currentGeneratorIndex = (currentGeneratorIndex + 1) % numGenerators;
    generators[currentGeneratorIndex]->reset();

    Serial.print(F("Switched to: "));
    Serial.println(generatorNames[currentGeneratorIndex]);
}

void updateCurrentGenerator(EffectMatrix& matrix, float audioEnergy, bool audioHit) {
    Generator* current = generators[currentGeneratorIndex];
    current->generate(matrix, audioEnergy, audioHit);
}

// Example: Customize generator parameters
void customizeGenerators() {
    // Customize Fire generator
    FireParams fireParams;
    fireParams.baseCooling = 100;        // Slower cooling for taller flames
    fireParams.sparkChance = 0.4f;       // More sparks
    fireParams.audioSparkBoost = 0.5f;   // Strong audio response
    fireGenerator.setParams(fireParams);

    // Customize Water generator
    WaterParams waterParams;
    waterParams.baseFlow = 80;           // Slower flow
    waterParams.waveChance = 0.3f;       // More waves
    waterParams.audioWaveBoost = 0.6f;   // Strong wave response
    waterGenerator.setParams(waterParams);

    // Customize Lightning generator
    LightningParams lightningParams;
    lightningParams.boltChance = 0.2f;       // More frequent bolts
    lightningParams.branchChance = 40;       // More branching
    lightningParams.audioBoltBoost = 0.7f;  // Very audio-reactive
    lightningGenerator.setParams(lightningParams);
}

// Example: Audio-reactive generator selection
void selectGeneratorByAudio(float audioEnergy) {
    static uint32_t lastSwitchMs = 0;
    uint32_t currentMs = millis();

    // Switch generators based on audio energy level
    if (currentMs - lastSwitchMs > 5000) { // Min 5 seconds between switches
        if (audioEnergy > 0.8f) {
            // High energy → Lightning
            if (currentGeneratorIndex != 2) {
                currentGeneratorIndex = 2;
                generators[currentGeneratorIndex]->reset();
                Serial.println(F("High energy → Lightning"));
            }
        } else if (audioEnergy > 0.4f) {
            // Medium energy → Fire
            if (currentGeneratorIndex != 0) {
                currentGeneratorIndex = 0;
                generators[currentGeneratorIndex]->reset();
                Serial.println(F("Medium energy → Fire"));
            }
        } else {
            // Low energy → Water
            if (currentGeneratorIndex != 1) {
                currentGeneratorIndex = 1;
                generators[currentGeneratorIndex]->reset();
                Serial.println(F("Low energy → Water"));
            }
        }
        lastSwitchMs = currentMs;
    }
}

/*
 * USAGE NOTES:
 *
 * 1. All generators work with any device layout:
 *    - Hat (LINEAR): String-based effects
 *    - Tube Light (MATRIX): 2D matrix effects
 *    - Bucket Totem (MATRIX): Large matrix effects
 *
 * 2. Color palettes:
 *    - Fire: Red → Orange → Yellow → White
 *    - Water: Deep Blue → Blue → Cyan → Light Blue
 *    - Lightning: Yellow → White → Electric Blue
 *
 * 3. Audio reactivity:
 *    - All generators respond to energy level (0.0-1.0)
 *    - Hit detection creates burst effects
 *    - Each generator has unique audio response characteristics
 *
 * 4. Performance:
 *    - Fire: ~20 FPS update rate
 *    - Water: ~20 FPS update rate
 *    - Lightning: ~33 FPS update rate (faster for bolt effects)
 */
