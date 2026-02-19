#pragma once
/**
 * ParamParser.h - Runtime parameter injection for simulator
 *
 * Parses key=value pairs and applies them to generator params.
 * Enables fast iteration without rebuilding.
 */

#include <string>
#include <map>
#include <sstream>
#include <vector>
#include <fstream>

// Parameter map type
using ParamMap = std::map<std::string, std::string>;

class ParamParser {
public:
    /**
     * Parse comma-separated key=value pairs
     * Example: "baseSpawnChance=0.15,gravity=-12,burstSparks=10"
     */
    static ParamMap parse(const std::string& paramString) {
        ParamMap params;
        if (paramString.empty()) return params;

        std::istringstream stream(paramString);
        std::string pair;

        while (std::getline(stream, pair, ',')) {
            size_t eqPos = pair.find('=');
            if (eqPos != std::string::npos) {
                std::string key = trim(pair.substr(0, eqPos));
                std::string value = trim(pair.substr(eqPos + 1));
                params[key] = value;
            }
        }
        return params;
    }

    /**
     * Get float value from param map
     */
    static float getFloat(const ParamMap& params, const std::string& key, float defaultVal) {
        auto it = params.find(key);
        if (it != params.end()) {
            return std::stof(it->second);
        }
        return defaultVal;
    }

    /**
     * Get int value from param map
     */
    static int getInt(const ParamMap& params, const std::string& key, int defaultVal) {
        auto it = params.find(key);
        if (it != params.end()) {
            return std::stoi(it->second);
        }
        return defaultVal;
    }

    /**
     * Check if param exists
     */
    static bool has(const ParamMap& params, const std::string& key) {
        return params.find(key) != params.end();
    }

    /**
     * Write params to JSON file
     */
    static void writeJson(const std::string& path, const std::string& generator,
                          const ParamMap& overrides, const ParamMap& allParams) {
        std::ofstream file(path);
        file << "{\n";
        file << "  \"generator\": \"" << generator << "\",\n";

        // Write overrides (what was changed)
        file << "  \"overrides\": {";
        bool first = true;
        for (const auto& p : overrides) {
            if (!first) file << ",";
            file << "\n    \"" << p.first << "\": " << p.second;
            first = false;
        }
        file << "\n  },\n";

        // Write all params (full state)
        file << "  \"params\": {";
        first = true;
        for (const auto& p : allParams) {
            if (!first) file << ",";
            file << "\n    \"" << p.first << "\": " << p.second;
            first = false;
        }
        file << "\n  }\n";
        file << "}\n";
        file.close();
    }

private:
    static std::string trim(const std::string& s) {
        size_t start = s.find_first_not_of(" \t");
        size_t end = s.find_last_not_of(" \t");
        return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    }
};

// Apply parsed params to FireParams
#include "../../blinky-things/generators/Fire.h"
inline void applyParams(FireParams& p, const ParamMap& params) {
    p.baseSpawnChance = ParamParser::getFloat(params, "baseSpawnChance", p.baseSpawnChance);
    p.audioSpawnBoost = ParamParser::getFloat(params, "audioSpawnBoost", p.audioSpawnBoost);
    p.maxParticles = ParamParser::getInt(params, "maxParticles", p.maxParticles);
    p.defaultLifespan = ParamParser::getInt(params, "defaultLifespan", p.defaultLifespan);
    p.intensityMin = ParamParser::getInt(params, "intensityMin", p.intensityMin);
    p.intensityMax = ParamParser::getInt(params, "intensityMax", p.intensityMax);
    p.gravity = ParamParser::getFloat(params, "gravity", p.gravity);
    p.windBase = ParamParser::getFloat(params, "windBase", p.windBase);
    p.windVariation = ParamParser::getFloat(params, "windVariation", p.windVariation);
    p.drag = ParamParser::getFloat(params, "drag", p.drag);
    p.sparkVelocityMin = ParamParser::getFloat(params, "sparkVelocityMin", p.sparkVelocityMin);
    p.sparkVelocityMax = ParamParser::getFloat(params, "sparkVelocityMax", p.sparkVelocityMax);
    p.sparkSpread = ParamParser::getFloat(params, "sparkSpread", p.sparkSpread);
    p.trailHeatFactor = ParamParser::getInt(params, "trailHeatFactor", p.trailHeatFactor);
    p.trailDecay = ParamParser::getInt(params, "trailDecay", p.trailDecay);
    p.musicSpawnPulse = ParamParser::getFloat(params, "musicSpawnPulse", p.musicSpawnPulse);
    p.organicTransientMin = ParamParser::getFloat(params, "organicTransientMin", p.organicTransientMin);
    p.burstSparks = ParamParser::getInt(params, "burstSparks", p.burstSparks);
}

// Get all FireParams as map (for JSON output)
inline ParamMap getParamMap(const FireParams& p) {
    ParamMap m;
    m["baseSpawnChance"] = std::to_string(p.baseSpawnChance);
    m["audioSpawnBoost"] = std::to_string(p.audioSpawnBoost);
    m["maxParticles"] = std::to_string(p.maxParticles);
    m["defaultLifespan"] = std::to_string(p.defaultLifespan);
    m["intensityMin"] = std::to_string(p.intensityMin);
    m["intensityMax"] = std::to_string(p.intensityMax);
    m["gravity"] = std::to_string(p.gravity);
    m["windBase"] = std::to_string(p.windBase);
    m["windVariation"] = std::to_string(p.windVariation);
    m["drag"] = std::to_string(p.drag);
    m["sparkVelocityMin"] = std::to_string(p.sparkVelocityMin);
    m["sparkVelocityMax"] = std::to_string(p.sparkVelocityMax);
    m["sparkSpread"] = std::to_string(p.sparkSpread);
    m["trailHeatFactor"] = std::to_string(p.trailHeatFactor);
    m["trailDecay"] = std::to_string(p.trailDecay);
    m["musicSpawnPulse"] = std::to_string(p.musicSpawnPulse);
    m["organicTransientMin"] = std::to_string(p.organicTransientMin);
    m["burstSparks"] = std::to_string(p.burstSparks);
    return m;
}

// Apply parsed params to WaterParams
#include "../../blinky-things/generators/Water.h"
inline void applyParams(WaterParams& p, const ParamMap& params) {
    p.baseSpawnChance = ParamParser::getFloat(params, "baseSpawnChance", p.baseSpawnChance);
    p.audioSpawnBoost = ParamParser::getFloat(params, "audioSpawnBoost", p.audioSpawnBoost);
    p.maxParticles = ParamParser::getInt(params, "maxParticles", p.maxParticles);
    p.defaultLifespan = ParamParser::getInt(params, "defaultLifespan", p.defaultLifespan);
    p.intensityMin = ParamParser::getInt(params, "intensityMin", p.intensityMin);
    p.intensityMax = ParamParser::getInt(params, "intensityMax", p.intensityMax);
    p.gravity = ParamParser::getFloat(params, "gravity", p.gravity);
    p.windBase = ParamParser::getFloat(params, "windBase", p.windBase);
    p.windVariation = ParamParser::getFloat(params, "windVariation", p.windVariation);
    p.drag = ParamParser::getFloat(params, "drag", p.drag);
    p.dropVelocityMin = ParamParser::getFloat(params, "dropVelocityMin", p.dropVelocityMin);
    p.dropVelocityMax = ParamParser::getFloat(params, "dropVelocityMax", p.dropVelocityMax);
    p.dropSpread = ParamParser::getFloat(params, "dropSpread", p.dropSpread);
    p.splashParticles = ParamParser::getInt(params, "splashParticles", p.splashParticles);
    p.splashVelocityMin = ParamParser::getFloat(params, "splashVelocityMin", p.splashVelocityMin);
    p.splashVelocityMax = ParamParser::getFloat(params, "splashVelocityMax", p.splashVelocityMax);
    p.splashIntensity = ParamParser::getInt(params, "splashIntensity", p.splashIntensity);
    p.musicSpawnPulse = ParamParser::getFloat(params, "musicSpawnPulse", p.musicSpawnPulse);
    p.organicTransientMin = ParamParser::getFloat(params, "organicTransientMin", p.organicTransientMin);
}

inline ParamMap getParamMap(const WaterParams& p) {
    ParamMap m;
    m["baseSpawnChance"] = std::to_string(p.baseSpawnChance);
    m["audioSpawnBoost"] = std::to_string(p.audioSpawnBoost);
    m["maxParticles"] = std::to_string(p.maxParticles);
    m["defaultLifespan"] = std::to_string(p.defaultLifespan);
    m["intensityMin"] = std::to_string(p.intensityMin);
    m["intensityMax"] = std::to_string(p.intensityMax);
    m["gravity"] = std::to_string(p.gravity);
    m["windBase"] = std::to_string(p.windBase);
    m["windVariation"] = std::to_string(p.windVariation);
    m["drag"] = std::to_string(p.drag);
    m["dropVelocityMin"] = std::to_string(p.dropVelocityMin);
    m["dropVelocityMax"] = std::to_string(p.dropVelocityMax);
    m["dropSpread"] = std::to_string(p.dropSpread);
    m["splashParticles"] = std::to_string(p.splashParticles);
    m["splashVelocityMin"] = std::to_string(p.splashVelocityMin);
    m["splashVelocityMax"] = std::to_string(p.splashVelocityMax);
    m["splashIntensity"] = std::to_string(p.splashIntensity);
    m["musicSpawnPulse"] = std::to_string(p.musicSpawnPulse);
    m["organicTransientMin"] = std::to_string(p.organicTransientMin);
    return m;
}

// Apply parsed params to LightningParams
#include "../../blinky-things/generators/Lightning.h"
inline void applyParams(LightningParams& p, const ParamMap& params) {
    p.baseSpawnChance = ParamParser::getFloat(params, "baseSpawnChance", p.baseSpawnChance);
    p.audioSpawnBoost = ParamParser::getFloat(params, "audioSpawnBoost", p.audioSpawnBoost);
    p.maxParticles = ParamParser::getInt(params, "maxParticles", p.maxParticles);
    p.defaultLifespan = ParamParser::getInt(params, "defaultLifespan", p.defaultLifespan);
    p.intensityMin = ParamParser::getInt(params, "intensityMin", p.intensityMin);
    p.intensityMax = ParamParser::getInt(params, "intensityMax", p.intensityMax);
    p.boltVelocityMin = ParamParser::getFloat(params, "boltVelocityMin", p.boltVelocityMin);
    p.boltVelocityMax = ParamParser::getFloat(params, "boltVelocityMax", p.boltVelocityMax);
    p.fadeRate = ParamParser::getInt(params, "fadeRate", p.fadeRate);
    p.branchChance = ParamParser::getInt(params, "branchChance", p.branchChance);
    p.branchCount = ParamParser::getInt(params, "branchCount", p.branchCount);
    p.branchAngleSpread = ParamParser::getFloat(params, "branchAngleSpread", p.branchAngleSpread);
    p.branchIntensityLoss = ParamParser::getInt(params, "branchIntensityLoss", p.branchIntensityLoss);
    p.musicSpawnPulse = ParamParser::getFloat(params, "musicSpawnPulse", p.musicSpawnPulse);
    p.organicTransientMin = ParamParser::getFloat(params, "organicTransientMin", p.organicTransientMin);
}

inline ParamMap getParamMap(const LightningParams& p) {
    ParamMap m;
    m["baseSpawnChance"] = std::to_string(p.baseSpawnChance);
    m["audioSpawnBoost"] = std::to_string(p.audioSpawnBoost);
    m["maxParticles"] = std::to_string(p.maxParticles);
    m["defaultLifespan"] = std::to_string(p.defaultLifespan);
    m["intensityMin"] = std::to_string(p.intensityMin);
    m["intensityMax"] = std::to_string(p.intensityMax);
    m["boltVelocityMin"] = std::to_string(p.boltVelocityMin);
    m["boltVelocityMax"] = std::to_string(p.boltVelocityMax);
    m["fadeRate"] = std::to_string(p.fadeRate);
    m["branchChance"] = std::to_string(p.branchChance);
    m["branchCount"] = std::to_string(p.branchCount);
    m["branchAngleSpread"] = std::to_string(p.branchAngleSpread);
    m["branchIntensityLoss"] = std::to_string(p.branchIntensityLoss);
    m["musicSpawnPulse"] = std::to_string(p.musicSpawnPulse);
    m["organicTransientMin"] = std::to_string(p.organicTransientMin);
    return m;
}

// === AUDIO VISUALIZATION PARAMS ===
#include "../../blinky-things/generators/Audio.h"
inline void applyParams(AudioParams& p, const ParamMap& params) {
    p.transientRowFraction = ParamParser::getFloat(params, "transientRowFraction", p.transientRowFraction);
    p.transientDecayRate = ParamParser::getFloat(params, "transientDecayRate", p.transientDecayRate);
    p.transientBrightness = ParamParser::getInt(params, "transientBrightness", p.transientBrightness);
    p.levelBrightness = ParamParser::getInt(params, "levelBrightness", p.levelBrightness);
    p.levelSmoothing = ParamParser::getFloat(params, "levelSmoothing", p.levelSmoothing);
    p.phaseBrightness = ParamParser::getInt(params, "phaseBrightness", p.phaseBrightness);
    p.musicModeThreshold = ParamParser::getFloat(params, "musicModeThreshold", p.musicModeThreshold);
    p.beatPulseBrightness = ParamParser::getInt(params, "beatPulseBrightness", p.beatPulseBrightness);
    p.beatPulseDecay = ParamParser::getFloat(params, "beatPulseDecay", p.beatPulseDecay);
    p.beatPulseWidth = ParamParser::getFloat(params, "beatPulseWidth", p.beatPulseWidth);
    p.backgroundBrightness = ParamParser::getInt(params, "backgroundBrightness", p.backgroundBrightness);
}

inline ParamMap getParamMap(const AudioParams& p) {
    ParamMap m;
    m["transientRowFraction"] = std::to_string(p.transientRowFraction);
    m["transientDecayRate"] = std::to_string(p.transientDecayRate);
    m["transientBrightness"] = std::to_string(p.transientBrightness);
    m["levelBrightness"] = std::to_string(p.levelBrightness);
    m["levelSmoothing"] = std::to_string(p.levelSmoothing);
    m["phaseBrightness"] = std::to_string(p.phaseBrightness);
    m["musicModeThreshold"] = std::to_string(p.musicModeThreshold);
    m["beatPulseBrightness"] = std::to_string(p.beatPulseBrightness);
    m["beatPulseDecay"] = std::to_string(p.beatPulseDecay);
    m["beatPulseWidth"] = std::to_string(p.beatPulseWidth);
    m["backgroundBrightness"] = std::to_string(p.backgroundBrightness);
    return m;
}
