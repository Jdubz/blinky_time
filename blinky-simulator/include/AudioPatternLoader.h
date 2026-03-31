#pragma once
/**
 * AudioPatternLoader - Load and generate audio patterns for simulation
 *
 * Provides scripted AudioControl sequences for deterministic rendering.
 * Supports both programmatic patterns and JSON file loading.
 */

#include <cstdint>
#include <vector>
#include <string>
#include <cmath>
#include <fstream>
#include <sstream>

// Include AudioControl from blinky-things
#include "../../blinky-things/audio/AudioControl.h"

struct AudioKeyframe {
    uint32_t timeMs;
    float energy;
    float pulse;
    float phase;
    float rhythmStrength;
};

class AudioPattern {
private:
    std::vector<AudioKeyframe> keyframes_;
    std::string name_;
    float bpm_ = 120.0f;
    uint32_t durationMs_ = 0;

public:
    AudioPattern(const std::string& name = "unnamed") : name_(name) {}

    void setName(const std::string& name) { name_ = name; }
    const std::string& getName() const { return name_; }

    void setBPM(float bpm) { bpm_ = bpm; }
    float getBPM() const { return bpm_; }

    void setDuration(uint32_t ms) { durationMs_ = ms; }
    uint32_t getDuration() const { return durationMs_; }

    void addKeyframe(uint32_t timeMs, float energy, float pulse, float phase, float rhythmStrength) {
        keyframes_.push_back({timeMs, energy, pulse, phase, rhythmStrength});
        if (timeMs > durationMs_) {
            durationMs_ = timeMs;
        }
    }

    // Get interpolated audio state at time
    AudioControl getAudioAt(uint32_t timeMs) const {
        AudioControl audio;

        if (keyframes_.empty()) {
            return audio;
        }

        // Find surrounding keyframes
        const AudioKeyframe* prev = nullptr;
        const AudioKeyframe* next = nullptr;

        for (size_t i = 0; i < keyframes_.size(); i++) {
            if (keyframes_[i].timeMs <= timeMs) {
                prev = &keyframes_[i];
            }
            if (keyframes_[i].timeMs >= timeMs && !next) {
                next = &keyframes_[i];
                break;
            }
        }

        if (!prev && !next) {
            return audio;
        }

        if (!prev) {
            prev = next;
        }
        if (!next) {
            next = prev;
        }

        // Interpolate between keyframes
        if (prev == next || prev->timeMs == next->timeMs) {
            audio.energy = prev->energy;
            audio.pulse = prev->pulse;
            audio.phase = prev->phase;
            audio.rhythmStrength = prev->rhythmStrength;
        } else {
            float t = static_cast<float>(timeMs - prev->timeMs) / (next->timeMs - prev->timeMs);
            audio.energy = prev->energy + t * (next->energy - prev->energy);
            audio.pulse = prev->pulse + t * (next->pulse - prev->pulse);
            audio.phase = prev->phase + t * (next->phase - prev->phase);
            audio.rhythmStrength = prev->rhythmStrength + t * (next->rhythmStrength - prev->rhythmStrength);
        }

        return audio;
    }

    // Generate audio from BPM pattern
    void generateFromBPM(float bpm, uint32_t durationMs, float rhythmStrength = 0.8f) {
        bpm_ = bpm;
        durationMs_ = durationMs;
        keyframes_.clear();

        float beatIntervalMs = 60000.0f / bpm;
        float beatCount = 0;

        for (uint32_t t = 0; t <= durationMs; t += 16) { // ~60 FPS
            float timeSec = t / 1000.0f;
            float beatPhase = fmodf(timeSec * bpm / 60.0f, 1.0f);

            // Energy varies with phase
            float energy = 0.3f + 0.4f * (0.5f + 0.5f * cosf(beatPhase * 6.28318f));

            // Pulse on beat (phase near 0)
            float pulse = 0.0f;
            if (beatPhase < 0.1f) {
                pulse = 1.0f - beatPhase * 10.0f;
            }

            addKeyframe(t, energy, pulse, beatPhase, rhythmStrength);
        }
    }
};

class AudioPatternLoader {
public:
    // Built-in patterns
    static AudioPattern createSteadyBeat(float bpm, uint32_t durationMs) {
        AudioPattern pattern("steady-beat");
        pattern.generateFromBPM(bpm, durationMs);
        return pattern;
    }

    static AudioPattern createSilence(uint32_t durationMs) {
        AudioPattern pattern("silence");
        pattern.setDuration(durationMs);
        // Add keyframes with no audio
        for (uint32_t t = 0; t <= durationMs; t += 100) {
            pattern.addKeyframe(t, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        return pattern;
    }

    static AudioPattern createBurst(uint32_t durationMs, int burstCount = 5) {
        AudioPattern pattern("burst");
        pattern.setDuration(durationMs);

        uint32_t interval = durationMs / (burstCount + 1);
        for (int i = 0; i < burstCount; i++) {
            uint32_t burstTime = interval * (i + 1);

            // Ramp up to burst
            if (burstTime > 100) {
                pattern.addKeyframe(burstTime - 100, 0.2f, 0.0f, 0.0f, 0.0f);
            }

            // Burst peak
            pattern.addKeyframe(burstTime, 1.0f, 1.0f, 0.0f, 0.0f);

            // Decay after burst
            pattern.addKeyframe(burstTime + 50, 0.6f, 0.3f, 0.1f, 0.0f);
            pattern.addKeyframe(burstTime + 200, 0.2f, 0.0f, 0.4f, 0.0f);
        }

        return pattern;
    }

    static AudioPattern createComplex(uint32_t durationMs) {
        AudioPattern pattern("complex");
        pattern.setBPM(128.0f);
        pattern.setDuration(durationMs);

        // Complex pattern with varying rhythm strength
        for (uint32_t t = 0; t <= durationMs; t += 16) {
            float timeSec = t / 1000.0f;

            // Rhythm strength builds over time
            float rhythmStrength = std::min(1.0f, timeSec / 2.0f);

            // Phase from BPM
            float beatPhase = fmodf(timeSec * 128.0f / 60.0f, 1.0f);

            // Energy with variation
            float energy = 0.4f + 0.3f * sinf(timeSec * 2.0f) + 0.2f * cosf(beatPhase * 6.28f);

            // Pulse on beat
            float pulse = (beatPhase < 0.1f) ? (1.0f - beatPhase * 10.0f) : 0.0f;

            // Add extra pulses on off-beats sometimes
            if (rhythmStrength > 0.5f && beatPhase > 0.45f && beatPhase < 0.55f) {
                pulse = 0.5f;
            }

            pattern.addKeyframe(t, energy, pulse, beatPhase, rhythmStrength);
        }

        return pattern;
    }

    // Load pattern from simple text format
    static AudioPattern loadFromFile(const std::string& filename) {
        AudioPattern pattern(filename);

        std::ifstream file(filename);
        if (!file.is_open()) {
            // Return empty pattern
            return pattern;
        }

        std::string line;
        while (std::getline(file, line)) {
            // Skip comments and empty lines
            if (line.empty() || line[0] == '#') continue;

            // Parse "time,energy,pulse,phase,rhythm" format
            std::stringstream ss(line);
            std::string token;
            std::vector<float> values;

            while (std::getline(ss, token, ',')) {
                try {
                    values.push_back(std::stof(token));
                } catch (...) {
                    break;
                }
            }

            if (values.size() >= 5) {
                pattern.addKeyframe(
                    static_cast<uint32_t>(values[0]),
                    values[1], values[2], values[3], values[4]
                );
            } else if (values.size() >= 2) {
                // Simplified format: time,bpm
                float bpm = values[1];
                pattern.setBPM(bpm);
            }
        }

        return pattern;
    }

    // Get pattern by name
    // Realistic audio: quiet intro, music builds, drops, dynamic energy.
    // Models what the mic actually captures — noisy energy baseline, imperfect
    // pulse detection, variable onset density. No rhythmStrength (tempogram disabled).
    static AudioPattern createRealistic(uint32_t durationMs) {
        AudioPattern pattern("realistic");
        pattern.setBPM(126.0f);
        pattern.setDuration(durationMs);

        // Simple PRNG for deterministic "random" variation
        uint32_t seed = 12345;
        auto rng = [&seed]() -> float {
            seed = seed * 1664525 + 1013904223;
            return (seed >> 16) / 65535.0f;
        };

        for (uint32_t t = 0; t <= durationMs; t += 20) { // 50 FPS
            float timeSec = t / 1000.0f;

            // Section structure: quiet(0-3s) → build(3-6s) → drop(6-8s) → full(8-15s) → breakdown(15-18s) → outro(18+)
            float sectionGain;
            if (timeSec < 3.0f)       sectionGain = 0.15f + 0.05f * rng();  // Quiet ambient
            else if (timeSec < 6.0f)  sectionGain = 0.15f + (timeSec - 3.0f) / 3.0f * 0.55f; // Build
            else if (timeSec < 8.0f)  sectionGain = 0.3f + 0.1f * rng();    // Drop (quiet before drop)
            else if (timeSec < 15.0f) sectionGain = 0.7f + 0.3f * rng();    // Full energy
            else if (timeSec < 18.0f) sectionGain = 0.7f - (timeSec - 15.0f) / 3.0f * 0.4f; // Breakdown
            else                      sectionGain = 0.2f + 0.1f * rng();    // Outro

            // Energy: section gain + beat-aligned oscillation + noise
            float beatPhase = fmodf(timeSec * 126.0f / 60.0f, 1.0f);
            float beatOsc = 0.5f + 0.5f * cosf(beatPhase * 6.28318f);
            float noise = (rng() - 0.5f) * 0.15f;
            float energy = sectionGain * (0.6f + 0.4f * beatOsc) + noise;
            energy = std::max(0.0f, std::min(1.0f, energy));

            // Pulse: imperfect detection. Fires on ~70% of beats, sometimes slightly off-beat.
            // Spectral flux-style: sharp spike that decays quickly.
            float pulse = 0.0f;
            bool isBeatRegion = beatPhase < 0.08f;
            bool detected = rng() < 0.7f;  // 70% detection rate
            if (isBeatRegion && detected && sectionGain > 0.3f) {
                pulse = (0.5f + 0.5f * rng()) * sectionGain;  // Strength varies
            }
            // Occasional false positives
            if (rng() < 0.02f && sectionGain > 0.4f) {
                pulse = 0.2f * rng();
            }

            // No rhythmStrength (tempogram disabled in current firmware)
            pattern.addKeyframe(t, energy, pulse, beatPhase, 0.0f);
        }

        return pattern;
    }

    // Ambient: no music, just mic noise floor with occasional random spikes
    static AudioPattern createAmbient(uint32_t durationMs) {
        AudioPattern pattern("ambient");
        pattern.setDuration(durationMs);

        uint32_t seed = 67890;
        auto rng = [&seed]() -> float {
            seed = seed * 1664525 + 1013904223;
            return (seed >> 16) / 65535.0f;
        };

        for (uint32_t t = 0; t <= durationMs; t += 20) {
            float noise = 0.05f + 0.1f * rng();
            float spike = (rng() < 0.01f) ? 0.3f * rng() : 0.0f;  // Rare random spike
            float energy = noise + spike;
            pattern.addKeyframe(t, energy, 0.0f, 0.0f, 0.0f);
        }

        return pattern;
    }

    static AudioPattern getPattern(const std::string& name, uint32_t durationMs = 3000) {
        if (name == "steady-120bpm" || name == "steady") {
            return createSteadyBeat(120.0f, durationMs);
        } else if (name == "steady-90bpm") {
            return createSteadyBeat(90.0f, durationMs);
        } else if (name == "steady-140bpm" || name == "fast") {
            return createSteadyBeat(140.0f, durationMs);
        } else if (name == "silence" || name == "silent") {
            return createSilence(durationMs);
        } else if (name == "burst" || name == "bursts") {
            return createBurst(durationMs);
        } else if (name == "complex") {
            return createComplex(durationMs);
        } else if (name == "realistic" || name == "real") {
            return createRealistic(durationMs);
        } else if (name == "ambient" || name == "quiet") {
            return createAmbient(durationMs);
        } else {
            // Try to load from file
            AudioPattern pattern = loadFromFile(name);
            if (pattern.getDuration() == 0) {
                // Fall back to steady beat
                return createSteadyBeat(120.0f, durationMs);
            }
            return pattern;
        }
    }
};
