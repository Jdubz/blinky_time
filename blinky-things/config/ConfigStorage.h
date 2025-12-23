#pragma once

#include <Arduino.h>
#include "../generators/Fire.h"
#include "../inputs/AdaptiveMic.h"

// Simple configuration storage with flash persistence for nRF52
class ConfigStorage {
public:
    static const uint16_t MAGIC_NUMBER = 0x8F1E;
    static const uint8_t CONFIG_VERSION = 10;  // Peak-based AGC: removed agTarget (always 1.0), agMin/agMax/agcTauSeconds

    struct StoredFireParams {
        uint8_t baseCooling;
        uint8_t sparkHeatMin;
        uint8_t sparkHeatMax;
        float sparkChance;
        float audioSparkBoost;
        uint8_t audioHeatBoostMax;
        int8_t coolingAudioBias;
        uint8_t transientHeatMax;
        uint8_t spreadDistance;
        float heatDecay;
        uint16_t suppressionMs;
    };

    struct StoredMicParams {
        float noiseGate;
        float globalGain;
        float transientFactor;
        float loudFloor;
        // AGC time constants (peak target always 1.0)
        float agcAttackTau;       // Peak envelope attack
        float agcReleaseTau;      // Peak envelope release
        float agcGainTau;         // Gain adjustment speed
        // Timing parameters
        uint32_t transientCooldownMs;
        uint32_t hwCalibPeriodMs;
    };

    struct ConfigData {
        uint16_t magic;
        uint8_t version;
        StoredFireParams fire;
        StoredMicParams mic;
        uint8_t brightness;
    };

    ConfigStorage();
    void begin();
    bool isValid() const { return valid_; }

    void loadConfiguration(FireParams& fireParams, AdaptiveMic& mic);
    void saveConfiguration(const FireParams& fireParams, const AdaptiveMic& mic);
    void factoryReset();

    // Auto-save support
    void markDirty() { dirty_ = true; }
    void saveIfDirty(const FireParams& fireParams, const AdaptiveMic& mic);

private:
    ConfigData data_;
    bool valid_;
    bool dirty_;
    uint32_t lastSaveMs_;

    bool loadFromFlash();
    void saveToFlash();
    void loadDefaults();
};
