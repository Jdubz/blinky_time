#include "SerialConsole.h"
#include "../hal/PlatformDetect.h"
#include "../types/BlinkyAssert.h"
#include "../config/TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "../audio/AudioTracker.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../config/DeviceConfigLoader.h"  // v28: Runtime device config loading
#include "../types/Version.h"
#include "../render/RenderPipeline.h"
#include "../effects/HueRotationEffect.h"
#include <ArduinoJson.h>  // v28: JSON parsing for device config upload
#ifdef BLINKY_PLATFORM_NRF52840
#include "../comms/BleScanner.h"
#include "../comms/BleNus.h"
#elif defined(BLINKY_PLATFORM_ESP32S3)
#include "../comms/BleAdvertiser.h"
#include "../comms/WifiManager.h"
#endif

extern DeviceConfig config;  // v28: Changed to non-const for runtime loading

// Static instance for callbacks
SerialConsole* SerialConsole::instance_ = nullptr;

// Static debug channels - default to NONE (no debug output)
DebugChannel SerialConsole::debugChannels_ = DebugChannel::NONE;

// Callback for marking config dirty when parameters change
void onParamChanged() {
    if (SerialConsole::instance_) {
        ConfigStorage* storage = SerialConsole::instance_->getConfigStorage();
        if (storage) {
            storage->markDirty();
        }

        // CRITICAL: Update force adapters when wind/gravity/drag params change
        // The force adapter caches wind values via setWind(), so we must re-sync them
        Fire* fireGen = SerialConsole::instance_->fireGenerator_;
        Water* waterGen = SerialConsole::instance_->waterGenerator_;

        if (fireGen) {
            fireGen->syncPhysicsParams();
        }
        if (waterGen) {
            waterGen->syncPhysicsParams();
        }
    }
}

// (onHiResBassChanged removed v67 — BandFlux pipeline removed)

// File-scope storage for effect settings (accessible from both register and sync functions)
static float effectHueShift_ = 0.0f;
static float effectRotationSpeed_ = 0.0f;

// New constructor with RenderPipeline
SerialConsole::SerialConsole(RenderPipeline* pipeline, AdaptiveMic* mic)
    : pipeline_(pipeline), fireGenerator_(nullptr), heatFireGenerator_(nullptr),
      waterGenerator_(nullptr),
      lightningGenerator_(nullptr), audioVisGenerator_(nullptr), hueEffect_(nullptr), mic_(mic),
      battery_(nullptr), audioCtrl_(nullptr), configStorage_(nullptr) {
    instance_ = this;
    // Get generator pointers from pipeline
    if (pipeline_) {
        fireGenerator_ = pipeline_->getFireGenerator();
        heatFireGenerator_ = pipeline_->getHeatFireGenerator();
        waterGenerator_ = pipeline_->getWaterGenerator();
        lightningGenerator_ = pipeline_->getLightningGenerator();
        audioVisGenerator_ = pipeline_->getAudioVisGenerator();
        hueEffect_ = pipeline_->getHueRotationEffect();
    }
}

#ifdef BLINKY_PLATFORM_NRF52840
void SerialConsole::setBleNus(BleNus* nus) {
    bleNus_ = nus;
    if (nus) {
        // Tee output to both USB Serial and BLE NUS
        out_.setSecondary(nus);  // BleNus is-a Print (writes to paced ring buffer)
        settings_.setOutput(&out_);
    } else {
        out_.setSecondary(nullptr);
        settings_.setOutput(&out_);
    }
}
#endif

void SerialConsole::begin() {
    // Note: Serial.begin() should be called by main setup() before this
    settings_.setOutput(&out_);  // Route settings output through our tee
    settings_.begin();
    registerSettings();

    out_.println(F("Serial console ready."));
}

void SerialConsole::registerSettings() {
    // Get direct pointers to the fire generator's params
    FireParams* fp = nullptr;
    if (fireGenerator_) {
        fp = &fireGenerator_->getParamsMutable();
    }

    // Register all settings by category
    registerFireSettings(fp);

    // Register HeatFire generator settings
    if (heatFireGenerator_) {
        registerHeatFireSettings(&heatFireGenerator_->getParamsMutable());
    }

    // Register Water generator settings (use mutable ref so changes apply directly)
    if (waterGenerator_) {
        registerWaterSettings(&waterGenerator_->getParamsMutable());
    }

    // Register Lightning generator settings (use mutable ref so changes apply directly)
    if (lightningGenerator_) {
        registerLightningSettings(&lightningGenerator_->getParamsMutable());
    }

    // Register Audio visualization generator settings
    if (audioVisGenerator_) {
        registerAudioVisSettings(&audioVisGenerator_->getParamsMutable());
    }

    // Register effect settings (HueRotation)
    registerEffectSettings();

    // Audio settings
    registerAudioSettings();
    registerAgcSettings();
    // (registerTransientSettings/registerDetectionSettings/registerEnsembleSettings removed v67)
    // (registerRhythmSettings removed v74 — replaced by AudioTracker)
    registerTrackerSettings();
}

// === FIRE SETTINGS (Particle-based) ===
void SerialConsole::registerFireSettings(FireParams* fp) {
    if (!fp) return;

    // Spawn behavior
    settings_.registerFloat("basespawnchance", &fp->baseSpawnChance, "fire",
        "Baseline spark spawn probability", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("audiospawnboost", &fp->audioSpawnBoost, "fire",
        "Audio reactivity multiplier", 0.0f, 2.0f, onParamChanged);
    settings_.registerFloat("burstsparks", &fp->burstSparks, "fire",
        "Burst sparks (x crossDim -> count)", 0.1f, 2.0f, onParamChanged);

    // Physics
    settings_.registerFloat("gravity", &fp->gravity, "fire",
        "Gravity (x traversalDim -> LEDs/sec^2, neg=up)", -10.0f, 10.0f, onParamChanged);
    settings_.registerFloat("windbase", &fp->windBase, "fire",
        "Base wind force", -50.0f, 50.0f, onParamChanged);
    settings_.registerFloat("windvariation", &fp->windVariation, "fire",
        "Wind variation (x crossDim -> LEDs/sec)", 0.0f, 10.0f, onParamChanged);
    settings_.registerFloat("drag", &fp->drag, "fire",
        "Drag coefficient", 0.0f, 1.0f, onParamChanged);

    // Spark appearance
    settings_.registerFloat("sparkvelmin", &fp->sparkVelocityMin, "fire",
        "Min velocity (x traversalDim -> LEDs/sec)", 0.0f, 2.0f, onParamChanged);
    settings_.registerFloat("sparkvelmax", &fp->sparkVelocityMax, "fire",
        "Max velocity (x traversalDim -> LEDs/sec)", 0.0f, 2.0f, onParamChanged);
    settings_.registerFloat("sparkspread", &fp->sparkSpread, "fire",
        "Spread (x crossDim -> LEDs/sec)", 0.0f, 5.0f, onParamChanged);

    // Lifecycle
    settings_.registerFloat("maxparticles", &fp->maxParticles, "fire",
        "Max particles (× numLeds, clamped to pool)", 0.1f, 1.0f, onParamChanged);
    settings_.registerUint8("defaultlifespan", &fp->defaultLifespan, "fire",
        "Default particle lifespan (centiseconds, 100=1s)", 1, 255, onParamChanged);
    settings_.registerUint8("intensitymin", &fp->intensityMin, "fire",
        "Minimum spawn intensity", 0, 255, onParamChanged);
    settings_.registerUint8("intensitymax", &fp->intensityMax, "fire",
        "Maximum spawn intensity", 0, 255, onParamChanged);

    // Audio reactivity
    settings_.registerFloat("musicspawnpulse", &fp->musicSpawnPulse, "fire",
        "Beat spawn depth (0=flat, 1=full breathing)", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("organictransmin", &fp->organicTransientMin, "fire",
        "Min transient to trigger burst", 0.0f, 1.0f, onParamChanged);

    // Background
    settings_.registerFloat("bgintensity", &fp->backgroundIntensity, "fire",
        "Noise background brightness", 0.0f, 1.0f, onParamChanged);

    // Particle variety
    settings_.registerFloat("fastsparks", &fp->fastSparkRatio, "fire",
        "Fast spark ratio (0=all embers, 1=all sparks)", 0.0f, 1.0f, onParamChanged);

    // Thermal physics
    settings_.registerFloat("thermalforce", &fp->thermalForce, "fire",
        "Thermal buoyancy (x traversalDim -> LEDs/sec^2)", 0.0f, 10.0f, onParamChanged);
}

// === HEAT FIRE SETTINGS (Noise-field fire) ===
// Prefixed with "hf_" to avoid name collisions with particle fire settings.
void SerialConsole::registerHeatFireSettings(HeatFireParams* hfp) {
    if (!hfp) return;

    auto heatFireParamChanged = []() {};  // Noise-field params take effect immediately

    // Intensity threshold
    settings_.registerFloat("hf_silencethresh", &hfp->silenceThreshold, "heatfire",
        "Noise threshold at silence (higher = less fire)", 0.3f, 0.8f, heatFireParamChanged);
    settings_.registerFloat("hf_energydrop", &hfp->energyThresholdDrop, "heatfire",
        "Max threshold reduction from energy", 0.1f, 0.6f, heatFireParamChanged);
    settings_.registerFloat("hf_beatpulsedepth", &hfp->beatPulseDepth, "heatfire",
        "Phase breathing height amplitude", 0.0f, 0.5f, heatFireParamChanged);
    settings_.registerFloat("hf_burststrength", &hfp->burstStrength, "heatfire",
        "Transient pulse height flare", 0.0f, 0.5f, heatFireParamChanged);
    settings_.registerFloat("hf_organictransmin", &hfp->organicTransientMin, "heatfire",
        "Min transient to trigger burst", 0.0f, 1.0f, heatFireParamChanged);

    // Flame shape
    settings_.registerFloat("hf_flamebaseheight", &hfp->flameBaseHeight, "heatfire",
        "Flame height fraction at silence (0-1)", 0.1f, 0.8f, heatFireParamChanged);
    settings_.registerFloat("hf_warpstrength", &hfp->warpStrength, "heatfire",
        "Domain warp amplitude (lateral sway)", 0.0f, 1.0f, heatFireParamChanged);

    // Animation
    settings_.registerFloat("hf_noisespeed", &hfp->noiseSpeed, "heatfire",
        "Base noise scroll speed (units/sec)", 1.0f, 30.0f, heatFireParamChanged);
    settings_.registerFloat("hf_musicbeatdepth", &hfp->musicBeatDepth, "heatfire",
        "Beat sync depth for scroll speed (0-1)", 0.0f, 1.0f, heatFireParamChanged);
    settings_.registerFloat("hf_densityscrollboost", &hfp->densityScrollBoost, "heatfire",
        "OnsetDensity extra scroll speed (0=none, 1=+100%)", 0.0f, 1.0f, heatFireParamChanged);

    // Output
    settings_.registerFloat("hf_brightness", &hfp->brightness, "heatfire",
        "Master output brightness (0-1)", 0.0f, 1.0f, heatFireParamChanged);
}

// === AUDIO SETTINGS ===
// Window/Range normalization: peak/valley tracking adapts to signal
void SerialConsole::registerAudioSettings() {
    if (!mic_) return;

    settings_.registerFloat("peaktau", &mic_->peakTau, "audio",
        "Peak adaptation speed (s)", 0.5f, 10.0f);
    settings_.registerFloat("releasetau", &mic_->releaseTau, "audio",
        "Peak release speed (s)", 1.0f, 30.0f);
}

// === GAIN SETTINGS (v72: AGC removed, hardware gain fixed at platform default) ===
void SerialConsole::registerAgcSettings() {
    // AGC removed (v72) — hardware gain is fixed at platform optimal level.
    // Window/range normalization handles all dynamic range adaptation.
    // No user-tunable AGC parameters remain.
    // Function retained for ConfigStorage compatibility.
}

// (registerTransientSettings/registerDetectionSettings/registerEnsembleSettings removed v67 — BandFlux pipeline removed)

// === TRACKER SETTINGS (AudioTracker — ACF+PLP, v80) ===
void SerialConsole::registerTrackerSettings() {
    if (!audioCtrl_) return;

    // Tempo range
    settings_.registerFloat("bpmmin", &audioCtrl_->bpmMin, "tracker",
        "Minimum detectable BPM", 40.0f, 120.0f, onParamChanged);
    settings_.registerFloat("bpmmax", &audioCtrl_->bpmMax, "tracker",
        "Maximum detectable BPM", 120.0f, 240.0f, onParamChanged);
    // (rayleighBpm + combFeedback removed v80 — comb filter bank removed)

    // PLP pattern-learned pulse
    settings_.registerFloat("plpconfalpha", &audioCtrl_->plpConfAlpha, "tracker",
        "PLP confidence EMA smoothing rate", 0.01f, 0.5f, onParamChanged);
    settings_.registerFloat("plpnovgain", &audioCtrl_->plpNovGain, "tracker",
        "PLP pattern novelty scaling", 0.1f, 5.0f, onParamChanged);
    settings_.registerFloat("plpsigfloor", &audioCtrl_->plpSignalFloor, "tracker",
        "Mic level for full PLP confidence", 0.01f, 0.5f, onParamChanged);

    // Rhythm activation
    settings_.registerFloat("activationthreshold", &audioCtrl_->activationThreshold, "tracker",
        "Minimum periodicity to activate rhythm mode", 0.0f, 1.0f, onParamChanged);
    // Tempo smoothing
    settings_.registerFloat("temposmooth", &audioCtrl_->tempoSmoothing, "tracker",
        "BPM EMA smoothing factor (higher=slower)", 0.5f, 0.99f, onParamChanged);

    // (Phase-aware onset confidence modulation removed v78 — replaced by PLP)

    // NN profiling
    settings_.registerBool("nnprofile", &audioCtrl_->nnProfile, "tracker",
        "Enable NN inference profiling output");

    // === Newly exposed tuning constants (v74) ===
    // Spectral flux contrast
    settings_.registerFloat("odfcontrast", &audioCtrl_->odfContrast, "tracker",
        "Spectral flux contrast exponent (power-law sharpening)", 0.1f, 4.0f, onParamChanged);

    // Pulse detection
    settings_.registerFloat("pulsethreshmult", &audioCtrl_->pulseThresholdMult, "tracker",
        "Pulse baseline threshold multiplier", 1.0f, 5.0f, onParamChanged);
    settings_.registerFloat("pulseminlevel", &audioCtrl_->pulseMinLevel, "tracker",
        "Minimum mic level for pulse detection", 0.0f, 0.2f, onParamChanged);

    // Pulse detection tuning
    settings_.registerFloat("pulseonsetfloor", &audioCtrl_->pulseOnsetFloor, "tracker",
        "ODF floor for pulse detection scaling", 0.0f, 0.5f, onParamChanged);

    // (Percival ACF harmonic enhancement removed v80 — percival2/percival4)

    // ODF baseline tracking
    settings_.registerFloat("blfastdrop", &audioCtrl_->baselineFastDrop, "tracker",
        "ODF baseline fast drop rate", 0.01f, 0.2f, onParamChanged);
    settings_.registerFloat("blslowrise", &audioCtrl_->baselineSlowRise, "tracker",
        "ODF baseline slow rise rate", 0.001f, 0.05f, onParamChanged);
    settings_.registerFloat("odfpkdecay", &audioCtrl_->odfPeakHoldDecay, "tracker",
        "ODF peak-hold decay rate", 0.5f, 0.99f, onParamChanged);

    // Energy synthesis
    settings_.registerFloat("emicweight", &audioCtrl_->energyMicWeight, "tracker",
        "Energy: mic level weight", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("emelweight", &audioCtrl_->energyMelWeight, "tracker",
        "Energy: bass mel weight", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("eodfweight", &audioCtrl_->energyOdfWeight, "tracker",
        "Energy: ODF peak-hold weight", 0.0f, 1.0f, onParamChanged);
    // Spectral flux band weights (on SharedSpectralAnalysis, accessed via tracker)
    settings_.registerFloat("bassflux", &audioCtrl_->getSpectral().bassFluxWeight, "tracker",
        "Spectral flux: bass band weight (62-375Hz)", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("midflux", &audioCtrl_->getSpectral().midFluxWeight, "tracker",
        "Spectral flux: mid band weight (437-2000Hz)", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("highflux", &audioCtrl_->getSpectral().highFluxWeight, "tracker",
        "Spectral flux: high band weight (2-8kHz)", 0.0f, 1.0f, onParamChanged);

    // Pattern slot cache (v82)
    settings_.registerFloat("slotswitchthresh", &audioCtrl_->slotSwitchThreshold, "slots",
        "Cosine similarity threshold to recall cached slot", 0.50f, 0.95f, onParamChanged);
    settings_.registerFloat("slotnewthresh", &audioCtrl_->slotNewThreshold, "slots",
        "Below this similarity: allocate new slot", 0.20f, 0.60f, onParamChanged);
    settings_.registerFloat("slotupdaterate", &audioCtrl_->slotUpdateRate, "slots",
        "EMA rate for reinforcing active slot", 0.05f, 0.40f, onParamChanged);
    settings_.registerFloat("slotsaveconf", &audioCtrl_->slotSaveMinConf, "slots",
        "Min PLP confidence to save/update slots", 0.20f, 0.80f, onParamChanged);
    settings_.registerFloat("slotseedblend", &audioCtrl_->slotSeedBlend, "slots",
        "Blend ratio when seeding from cached slot", 0.30f, 0.95f, onParamChanged);
}

// (registerRhythmSettings removed v74 — ~250 lines of CBSS/Bayesian settings. See git history.)

void SerialConsole::update() {
    // Handle incoming commands
    if (Serial.available()) {
        // Buffer must accommodate full device config JSON (~550 bytes)
        static char buf[768];
        size_t len = Serial.readBytesUntil('\n', buf, sizeof(buf) - 1);
        // Explicit bounds check for safety
        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        buf[len] = '\0';
        // Trim CR/LF
        while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n')) {
            buf[--len] = '\0';
        }
        if (len > 0) {
            handleCommand(buf);
        }
    }

    // JSON streaming for web app
    streamTick();
}

void SerialConsole::handleCommand(const char* cmd, Print& output) {
    // Temporarily redirect output to the specified Print target (e.g., TCP client).
    // Saves and restores the TeeStream's secondary to avoid permanent state change.
    Print* savedSecondary = out_.secondary();
    out_.setSecondary(&output);
    handleCommand(cmd);
    out_.setSecondary(savedSecondary);
}

void SerialConsole::handleCommand(const char* cmd) {
    // (handleEnsembleCommand dispatch removed v67 — BandFlux pipeline removed)

    // Check for beat tracking commands
    if (handleBeatTrackingCommand(cmd)) {
        return;
    }

    // Try settings registry (handles set/get/show/list/categories/settings)
    if (settings_.handleCommand(cmd)) {
        // Sync effect settings to actual effect after any settings change
        syncEffectSettings();
        // Warn about dangerous parameter interactions

        return;
    }

    // Then try special commands (JSON API, config management)
    if (handleSpecialCommand(cmd)) {
        return;
    }

    out_.println(F("Unknown command. Try 'settings' for help."));
}

bool SerialConsole::handleSpecialCommand(const char* cmd) {
    // Assert error counter
    if (strcmp(cmd, "show errors") == 0) {
        out_.print(F("{\"assertFails\":"));
        out_.print(BlinkyAssert::failCount);
        out_.println(F("}"));
        return true;
    }

    // Dispatch to specialized handlers (order matters for prefix matching)
    // NOTE: handleBeatTrackingCommand is called BEFORE settings registry
    // in handleCommand() to avoid "set" conflicts
    if (handleJsonCommand(cmd)) return true;
    if (handleGeneratorCommand(cmd)) return true;
    if (handleEffectCommand(cmd)) return true;
    if (handleBatteryCommand(cmd)) return true;
    if (handleStreamCommand(cmd)) return true;
    // cppcheck-suppress knownConditionTrueFalse -- stub for future test commands
    if (handleTestCommand(cmd)) return true;
    if (handleAudioStatusCommand(cmd)) return true;
    if (handleModeCommand(cmd)) return true;
    if (handleConfigCommand(cmd)) return true;
    if (handleDeviceConfigCommand(cmd)) return true;  // Device config commands (v28+)
    if (handleLogCommand(cmd)) return true;
    if (handleDebugCommand(cmd)) return true;     // Debug channel commands
    if (handleFakeAudioCommand(cmd)) return true; // Fake audio for visual debug
    if (handleBleCommand(cmd)) return true;       // BLE diagnostics
    if (handleWifiCommand(cmd)) return true;      // WiFi config (ESP32-S3)
    return false;
}

// === JSON API COMMANDS (for web app) ===
bool SerialConsole::handleJsonCommand(const char* cmd) {
    // Handle "json settings" or "json settings <category>"
    if (strncmp(cmd, "json settings", 13) == 0) {
        const char* category = cmd + 13;
        while (*category == ' ') category++;  // Skip whitespace

        if (*category == '\0') {
            settings_.printSettingsJson();    // All settings
        } else {
            settings_.printSettingsCategoryJson(category);  // Category only
        }
        return true;
    }

    if (strcmp(cmd, "json info") == 0) {
        out_.print(F("{\"version\":\""));
        out_.print(F(BLINKY_VERSION_STRING));
        out_.print(F("\""));

        // Device configuration status (v28+)
        if (configStorage_ && configStorage_->isDeviceConfigValid()) {
            const ConfigStorage::StoredDeviceConfig& cfg = configStorage_->getDeviceConfig();
            out_.print(F(",\"device\":{\"id\":\""));
            out_.print(cfg.deviceId);
            out_.print(F("\",\"name\":\""));
            out_.print(cfg.deviceName);
            out_.print(F("\",\"width\":"));
            out_.print(cfg.ledWidth);
            out_.print(F(",\"height\":"));
            out_.print(cfg.ledHeight);
            out_.print(F(",\"leds\":"));
            out_.print(cfg.ledWidth * cfg.ledHeight);
            out_.print(F(",\"configured\":true}"));
        } else {
            out_.print(F(",\"device\":{\"configured\":false,\"safeMode\":true}"));
        }

        out_.println(F("}"));
        return true;
    }

    if (strcmp(cmd, "json state") == 0) {
        if (!pipeline_) {
            out_.println(F("{\"error\":\"Pipeline not available\"}"));
            return true;
        }
        out_.print(F("{\"generator\":\""));
        out_.print(pipeline_->getGeneratorName());
        out_.print(F("\",\"effect\":\""));
        out_.print(pipeline_->getEffectName());
        out_.print(F("\",\"generators\":["));
        for (int i = 0; i < RenderPipeline::NUM_GENERATORS; i++) {
            if (i > 0) out_.print(',');
            out_.print('"');
            out_.print(RenderPipeline::getGeneratorNameByIndex(i));
            out_.print('"');
        }
        out_.print(F("],\"effects\":["));
        for (int i = 0; i < RenderPipeline::NUM_EFFECTS; i++) {
            if (i > 0) out_.print(',');
            out_.print('"');
            out_.print(RenderPipeline::getEffectNameByIndex(i));
            out_.print('"');
        }
        out_.println(F("]}"));
        return true;
    }

    return false;
}

// === BATTERY COMMANDS ===
bool SerialConsole::handleBatteryCommand(const char* cmd) {
    if (strcmp(cmd, "battery debug") == 0 || strcmp(cmd, "batt debug") == 0) {
        if (battery_) {
            out_.println(F("=== Battery Debug Info ==="));
            out_.print(F("Connected: "));
            out_.println(battery_->isBatteryConnected() ? F("Yes") : F("No"));
            out_.print(F("Voltage: "));
            out_.print(battery_->getVoltage(), 3);
            out_.println(F("V"));
            out_.print(F("Percent: "));
            out_.print(battery_->getPercent());
            out_.println(F("%"));
            out_.print(F("Charging: "));
            out_.println(battery_->isCharging() ? F("Yes") : F("No"));
            out_.println(F("(Use 'battery raw' for detailed ADC values)"));
        } else {
            out_.println(F("Battery monitor not available"));
        }
        return true;
    }

    if (strcmp(cmd, "battery") == 0 || strcmp(cmd, "batt") == 0) {
        if (battery_) {
            float voltage = battery_->getVoltage();
            uint8_t percent = battery_->getPercent();
            bool charging = battery_->isCharging();
            bool connected = battery_->isBatteryConnected();

            out_.print(F("{\"battery\":{"));
            out_.print(F("\"voltage\":"));
            out_.print(voltage, 2);
            out_.print(F(",\"percent\":"));
            out_.print(percent);
            out_.print(F(",\"charging\":"));
            out_.print(charging ? F("true") : F("false"));
            out_.print(F(",\"connected\":"));
            out_.print(connected ? F("true") : F("false"));
            out_.println(F("}}"));
        } else {
            out_.println(F("{\"error\":\"Battery monitor not available\"}"));
        }
        return true;
    }

    return false;
}

// === STREAM COMMANDS ===
bool SerialConsole::handleStreamCommand(const char* cmd) {
    if (strcmp(cmd, "stream on") == 0) {
        streamEnabled_ = true;
        out_.println(F("OK"));
        return true;
    }

    if (strcmp(cmd, "stream off") == 0) {
        streamEnabled_ = false;
        streamNN_ = false;
        out_.println(F("OK"));
        return true;
    }

    if (strcmp(cmd, "stream debug") == 0) {
        streamEnabled_ = true;
        streamDebug_ = true;
        out_.println(F("OK debug"));
        return true;
    }

    if (strcmp(cmd, "stream normal") == 0) {
        streamDebug_ = false;
        streamFast_ = false;
        streamNN_ = false;
        out_.println(F("OK normal"));
        return true;
    }

    if (strcmp(cmd, "stream nn") == 0) {
        streamEnabled_ = false;  // Disable timer-based stream to avoid TX overflow
        streamNN_ = true;        // NN stream fires independently on isFrameReady()
        streamFast_ = false;
        out_.println(F("OK nn"));
        return true;
    }

    if (strcmp(cmd, "stream fast") == 0) {
        streamEnabled_ = true;
        streamFast_ = true;
        out_.println(F("OK fast"));
        return true;
    }

    return false;
}

// === TEST MODE COMMANDS ===
bool SerialConsole::handleTestCommand(const char* cmd) {
    // Hardware gain lock/unlock removed (v72) — gain is fixed at platform optimal level.
    // Stub retained for future test commands.
    (void)cmd;
    return false;
}

// === AUDIO CONTROLLER STATUS ===
bool SerialConsole::handleAudioStatusCommand(const char* cmd) {
    if (strcmp(cmd, "music") == 0 || strcmp(cmd, "rhythm") == 0 || strcmp(cmd, "audio") == 0) {
        if (audioCtrl_) {
            const AudioControl& audio = audioCtrl_->getControl();
            out_.println(F("=== Audio Controller Status ==="));
            out_.print(F("Rhythm Active: "));
            out_.println(audio.rhythmStrength > audioCtrl_->activationThreshold ? F("YES") : F("NO"));
            out_.print(F("BPM: "));
            out_.println(audioCtrl_->getCurrentBpm(), 1);
            out_.print(F("Phase: "));
            out_.println(audio.phase, 2);
            out_.print(F("Rhythm Strength: "));
            out_.println(audio.rhythmStrength, 2);
            out_.print(F("Periodicity: "));
            out_.println(audioCtrl_->getPeriodicityStrength(), 2);
            out_.print(F("Energy: "));
            out_.println(audio.energy, 2);
            out_.print(F("Pulse: "));
            out_.println(audio.pulse, 2);
            out_.print(F("Onset Density: "));
            out_.print(audio.onsetDensity, 1);
            out_.println(F(" /s"));
            out_.print(F("BPM Range: "));
            out_.print(audioCtrl_->getBpmMin(), 0);
            out_.print(F("-"));
            out_.println(audioCtrl_->getBpmMax(), 0);

            // Advanced metrics
            out_.println(F("--- Advanced Metrics ---"));
            out_.print(F("Periodicity: "));
            out_.println(audioCtrl_->getPeriodicityStrength(), 2);
            out_.print(F("PLP Confidence: "));
            out_.println(audioCtrl_->getPlpConfidence(), 4);
        } else {
            out_.println(F("Audio controller not available"));
        }
        return true;
    }

    return false;
}

// === DETECTION MODE STATUS ===
bool SerialConsole::handleModeCommand(const char* cmd) {
    if (strcmp(cmd, "mode") == 0) {
        out_.println(F("=== Audio Detection Status ==="));
        if (audioCtrl_) {
            out_.print(F("Pulse Strength: "));
            out_.println(audioCtrl_->getLastPulseStrength(), 3);
            out_.print(F("BPM: "));
            out_.println(audioCtrl_->getCurrentBpm(), 1);
            out_.print(F("Rhythm Strength: "));
            out_.println(audioCtrl_->getPeriodicityStrength(), 3);
            out_.print(F("Beat Count: "));
            out_.println(audioCtrl_->getBeatCount());
        } else {
            out_.println(F("Audio controller not available"));
        }
        if (mic_) {
            out_.print(F("Audio Level: "));
            out_.println(mic_->getLevel(), 3);
            out_.print(F("Hardware Gain: "));
            out_.println(mic_->getHwGain());
        }
        return true;
    }

    return false;
}

// === CONFIGURATION COMMANDS ===
bool SerialConsole::handleConfigCommand(const char* cmd) {
    if (strcmp(cmd, "save") == 0) {
        if (configStorage_ && fireGenerator_ && waterGenerator_ && lightningGenerator_ && mic_) {
            configStorage_->saveConfiguration(
                fireGenerator_->getParams(),
                waterGenerator_->getParams(),
                lightningGenerator_->getParams(),
                *mic_,
                audioCtrl_
            );
            out_.println(F("OK"));
        } else {
            out_.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "load") == 0) {
        if (configStorage_ && fireGenerator_ && waterGenerator_ && lightningGenerator_ && mic_) {
            configStorage_->loadConfiguration(
                fireGenerator_->getParamsMutable(),
                waterGenerator_->getParamsMutable(),
                lightningGenerator_->getParamsMutable(),
                *mic_,
                audioCtrl_
            );
            out_.println(F("OK"));
        } else {
            out_.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "defaults") == 0) {
        restoreDefaults();

        out_.println(F("OK"));
        return true;
    }

    if (strcmp(cmd, "reset") == 0 || strcmp(cmd, "factory") == 0) {
        if (configStorage_) {
            configStorage_->factoryReset();
            restoreDefaults();
            out_.println(F("OK"));
        } else {
            out_.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "reboot") == 0) {
        out_.println(F("Rebooting..."));
        Serial.flush();  // Ensure message is sent before reset
        delay(100);      // Brief delay for serial transmission
#ifdef BLINKY_PLATFORM_NRF52840
        NVIC_SystemReset();
#elif defined(BLINKY_PLATFORM_ESP32S3)
        if (configStorage_) configStorage_->end();  // Flush NVS before restart
        ESP.restart();
#endif
        return true;  // Never reached
    }

    if (strcmp(cmd, "bootloader") == 0) {
#ifdef BLINKY_PLATFORM_NRF52840
        out_.println(F("Entering UF2 bootloader..."));
        Serial.flush();  // Ensure message is sent before reset
        delay(100);      // Brief delay for serial transmission
        // Set GPREGRET magic byte so the UF2 bootloader is entered on reset.
        // The Seeed/Adafruit non-mbed nRF52 core uses the SoftDevice — writing
        // GPREGRET directly is unreliable when the SoftDevice owns the POWER
        // peripheral (it clears the register during reset). Use the SD API when
        // the SoftDevice is active, otherwise write the register directly.
        // The mbed core does not link the SoftDevice API, so the inner guard
        // must remain as ARDUINO_ARCH_NRF52 (non-mbed core check).
        {
            const uint8_t DFU_MAGIC_UF2 = 0x57;
#ifdef ARDUINO_ARCH_NRF52
            uint8_t sd_en = 0;
            sd_softdevice_is_enabled(&sd_en);
            if (sd_en) {
                sd_power_gpregret_clr(0, 0xFF);
                sd_power_gpregret_set(0, DFU_MAGIC_UF2);
            } else {
                NRF_POWER->GPREGRET = DFU_MAGIC_UF2;
            }
#else
            // mbed core: SoftDevice API not available; write GPREGRET directly
            NRF_POWER->GPREGRET = DFU_MAGIC_UF2;
#endif
        }
        NVIC_SystemReset();
#else
        out_.println(F("UF2 bootloader not available on this platform"));
#endif
        return true;
    }

    return false;
}

// === DEVICE CONFIGURATION COMMANDS (v28+) ===
bool SerialConsole::handleDeviceConfigCommand(const char* cmd) {
    if (strcmp(cmd, "device show") == 0 || strcmp(cmd, "device") == 0) {
        showDeviceConfig();
        return true;
    }

    if (strncmp(cmd, "device upload ", 14) == 0) {
        uploadDeviceConfig(cmd + 14);
        return true;
    }

    out_.println(F("Device configuration commands:"));
    out_.println(F("  device show          - Display current device config"));
    out_.println(F("  device upload <JSON> - Upload device config from JSON"));
    out_.println(F("\nExample JSON at: devices/registry/README.md"));
    return false;
}

void SerialConsole::showDeviceConfig() {
    if (!configStorage_) {
        out_.println(F("{\"error\":\"ConfigStorage not available\"}"));
        return;
    }

    if (!configStorage_->isDeviceConfigValid()) {
        out_.println(F("{\"error\":\"No device config\",\"status\":\"unconfigured\",\"safeMode\":true}"));
        return;
    }

    const ConfigStorage::StoredDeviceConfig& cfg = configStorage_->getDeviceConfig();

    // Use ArduinoJson for clean, maintainable JSON serialization
    JsonDocument doc;

    // Device identification
    doc["deviceId"] = cfg.deviceId;
    doc["deviceName"] = cfg.deviceName;

    // Matrix/LED configuration
    doc["ledWidth"] = cfg.ledWidth;
    doc["ledHeight"] = cfg.ledHeight;
    doc["ledPin"] = cfg.ledPin;
    doc["brightness"] = cfg.brightness;
    doc["ledType"] = cfg.ledType;
    doc["orientation"] = cfg.orientation;
    doc["layoutType"] = cfg.layoutType;

    // Charging configuration
    doc["fastChargeEnabled"] = cfg.fastChargeEnabled;
    doc["lowBatteryThreshold"] = serialized(String(cfg.lowBatteryThreshold, 2));
    doc["criticalBatteryThreshold"] = serialized(String(cfg.criticalBatteryThreshold, 2));
    doc["minVoltage"] = serialized(String(cfg.minVoltage, 2));
    doc["maxVoltage"] = serialized(String(cfg.maxVoltage, 2));

    // IMU configuration
    doc["upVectorX"] = serialized(String(cfg.upVectorX, 2));
    doc["upVectorY"] = serialized(String(cfg.upVectorY, 2));
    doc["upVectorZ"] = serialized(String(cfg.upVectorZ, 2));
    doc["rotationDegrees"] = serialized(String(cfg.rotationDegrees, 2));
    doc["invertZ"] = cfg.invertZ;
    doc["swapXY"] = cfg.swapXY;
    doc["invertX"] = cfg.invertX;
    doc["invertY"] = cfg.invertY;

    // Serial configuration
    doc["baudRate"] = cfg.baudRate;
    doc["initTimeoutMs"] = cfg.initTimeoutMs;

    // Microphone configuration
    doc["sampleRate"] = cfg.sampleRate;
    doc["bufferSize"] = cfg.bufferSize;

    // Serialize with pretty printing for readability
    serializeJsonPretty(doc, Serial);
    out_.println();
}

void SerialConsole::uploadDeviceConfig(const char* jsonStr) {
    if (!configStorage_) {
        out_.println(F("ERROR: ConfigStorage not available"));
        return;
    }

    // Parse JSON using ArduinoJson (1024 bytes to accommodate full device configs ~600 bytes)
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonStr);

    if (error) {
        out_.print(F("ERROR: JSON parse failed - "));
        out_.println(error.c_str());
        out_.println(F("Example: device upload {\"deviceId\":\"hat_v1\",\"ledWidth\":89,...}"));
        return;
    }

    // Build StoredDeviceConfig from JSON
    ConfigStorage::StoredDeviceConfig newConfig = {};

    // Device identification
    strncpy(newConfig.deviceId, doc["deviceId"] | "unknown", sizeof(newConfig.deviceId) - 1);
    strncpy(newConfig.deviceName, doc["deviceName"] | "Unnamed Device", sizeof(newConfig.deviceName) - 1);

    // Matrix/LED configuration
    newConfig.ledWidth = doc["ledWidth"] | 0;
    newConfig.ledHeight = doc["ledHeight"] | 1;
    newConfig.ledPin = doc["ledPin"] | 10;
    newConfig.brightness = doc["brightness"] | 100;
    newConfig.ledType = doc["ledType"] | 12390;  // Default: NEO_GRB + NEO_KHZ800
    newConfig.orientation = doc["orientation"] | 0;
    newConfig.layoutType = doc["layoutType"] | 0;

    // Charging configuration
    newConfig.fastChargeEnabled = doc["fastChargeEnabled"] | false;
    newConfig.lowBatteryThreshold = doc["lowBatteryThreshold"] | 3.5f;
    newConfig.criticalBatteryThreshold = doc["criticalBatteryThreshold"] | 3.3f;
    newConfig.minVoltage = doc["minVoltage"] | 3.0f;
    newConfig.maxVoltage = doc["maxVoltage"] | 4.2f;

    // IMU configuration
    newConfig.upVectorX = doc["upVectorX"] | 0.0f;
    newConfig.upVectorY = doc["upVectorY"] | 0.0f;
    newConfig.upVectorZ = doc["upVectorZ"] | 1.0f;
    newConfig.rotationDegrees = doc["rotationDegrees"] | 0.0f;
    newConfig.invertZ = doc["invertZ"] | false;
    newConfig.swapXY = doc["swapXY"] | false;
    newConfig.invertX = doc["invertX"] | false;
    newConfig.invertY = doc["invertY"] | false;

    // Serial configuration
    newConfig.baudRate = doc["baudRate"] | 115200;
    newConfig.initTimeoutMs = doc["initTimeoutMs"] | 2000;

    // Microphone configuration
    newConfig.sampleRate = doc["sampleRate"] | 16000;
    newConfig.bufferSize = doc["bufferSize"] | 32;

    // Mark as valid
    newConfig.isValid = true;

    // Validate configuration
    if (!DeviceConfigLoader::validate(newConfig)) {
        out_.println(F("ERROR: Device config validation failed"));
        out_.println(F("Check LED count, pin numbers, and voltage ranges"));
        return;
    }

    // Save to flash
    configStorage_->setDeviceConfig(newConfig);

    // Trigger flash write by saving full configuration
    // Note: mic_ should always be available (audio initialized even in safe mode)
    // but generators may be null in safe mode
    if (fireGenerator_ && waterGenerator_ && lightningGenerator_ && mic_) {
        // Normal mode: save with actual generator params
        configStorage_->saveConfiguration(
            fireGenerator_->getParams(),
            waterGenerator_->getParams(),
            lightningGenerator_->getParams(),
            *mic_,
            audioCtrl_
        );
    } else if (mic_) {
        // Safe mode: generators null, but mic available
        // Save with default generator params (only device config matters)
        FireParams defaultFire;
        WaterParams defaultWater;
        LightningParams defaultLightning;
        configStorage_->saveConfiguration(
            defaultFire,
            defaultWater,
            defaultLightning,
            *mic_
        );
    } else {
        out_.println(F("ERROR: Cannot save config - mic not initialized"));
        return;
    }

    out_.println(F("✓ Device config saved to flash"));
    out_.print(F("Device: "));
    out_.print(newConfig.deviceName);
    out_.print(F(" ("));
    out_.print(newConfig.ledWidth * newConfig.ledHeight);
    out_.println(F(" LEDs)"));
    out_.println(F("\n**REBOOT DEVICE TO APPLY CONFIGURATION**"));
}

void SerialConsole::restoreDefaults() {
    // NOTE: Particle-based generators get defaults from their constructors
    // Generator parameter reset is handled by ConfigStorage::loadDefaults()
    // which will be applied on next load/save cycle

    // Restore mic defaults (window/range normalization)
    // v72: AGC removed — only window/range normalization tunables remain
    if (mic_) {
        mic_->peakTau = Defaults::PeakTau;              // 2s peak adaptation
        mic_->releaseTau = Defaults::ReleaseTau;        // 5s peak release
    }

    // Restore audio tracker defaults (all tunable params)
    if (audioCtrl_) {
        // Core tempo
        audioCtrl_->bpmMin = 15.0f;
        audioCtrl_->bpmMax = 200.0f;
        audioCtrl_->tempoSmoothing = 0.85f;
        audioCtrl_->activationThreshold = 0.3f;
        audioCtrl_->odfContrast = 1.25f;
        // PLP
        audioCtrl_->plpConfAlpha = 0.15f;
        audioCtrl_->plpNovGain = 1.5f;
        audioCtrl_->plpSignalFloor = 0.10f;
        // Pulse detection
        audioCtrl_->pulseThresholdMult = 2.0f;
        audioCtrl_->pulseMinLevel = 0.03f;
        audioCtrl_->pulseOnsetFloor = 0.1f;
        audioCtrl_->baselineFastDrop = 0.05f;
        audioCtrl_->baselineSlowRise = 0.005f;
        audioCtrl_->odfPeakHoldDecay = 0.85f;
        // Energy synthesis
        audioCtrl_->energyMicWeight = 0.30f;
        audioCtrl_->energyMelWeight = 0.30f;
        audioCtrl_->energyOdfWeight = 0.40f;
        // Pattern slot cache
        audioCtrl_->slotSwitchThreshold = 0.70f;
        audioCtrl_->slotNewThreshold = 0.40f;
        audioCtrl_->slotUpdateRate = 0.15f;
        audioCtrl_->slotSaveMinConf = 0.50f;
        audioCtrl_->slotSeedBlend = 0.70f;

        // Restore spectral processing defaults
        SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
        spectral.whitenEnabled = true;
        spectral.compressorEnabled = true;
        spectral.whitenDecay = 0.997f;
        spectral.whitenFloor = 0.001f;
        spectral.whitenBassBypass = false;
        spectral.compThresholdDb = -30.0f;
        spectral.compRatio = 3.0f;
        spectral.compKneeDb = 15.0f;
        spectral.compMakeupDb = 6.0f;
        spectral.compAttackTau = 0.001f;
        spectral.compReleaseTau = 2.0f;
        // Noise estimation (v56)
        spectral.noiseEstEnabled = false;
        spectral.noiseSmoothAlpha = 0.92f;
        spectral.noiseReleaseFactor = 0.999f;
        spectral.noiseOversubtract = 1.5f;
        spectral.noiseFloorRatio = 0.02f;

        // (BandFlux detector defaults removed v67 — BandFlux pipeline removed)
    }

    // Restore effect defaults
    if (hueEffect_) {
        hueEffect_->setHueShift(0.0f);
        hueEffect_->setRotationSpeed(0.0f);
    }
}

// === GENERATOR COMMANDS ===
bool SerialConsole::handleGeneratorCommand(const char* cmd) {
    if (!pipeline_) return false;  // Legitimately null in safe mode (no device config)

    // "gen list" - list available generators
    if (strcmp(cmd, "gen list") == 0 || strcmp(cmd, "gen") == 0) {
        out_.println(F("Available generators:"));
        for (int i = 0; i < RenderPipeline::NUM_GENERATORS; i++) {
            const char* name = RenderPipeline::getGeneratorNameByIndex(i);
            bool active = (RenderPipeline::getGeneratorTypeByIndex(i) == pipeline_->getGeneratorType());
            out_.print(F("  "));
            out_.print(name);
            if (active) out_.print(F(" (active)"));
            out_.println();
        }
        return true;
    }

    // "gen <name>" - switch to generator
    if (strncmp(cmd, "gen ", 4) == 0) {
        const char* name = cmd + 4;

        // Match generator by name
        GeneratorType type = GeneratorType::FIRE;  // Default
        bool found = false;

        if (strcmp(name, "fire") == 0) {
            type = GeneratorType::FIRE;
            found = true;
        } else if (strcmp(name, "water") == 0) {
            type = GeneratorType::WATER;
            found = true;
        } else if (strcmp(name, "lightning") == 0) {
            type = GeneratorType::LIGHTNING;
            found = true;
        } else if (strcmp(name, "audio") == 0) {
            type = GeneratorType::AUDIO;
            found = true;
        } else if (strcmp(name, "heatfire") == 0) {
            type = GeneratorType::HEAT_FIRE;
            found = true;
        }

        if (found) {
            if (pipeline_->setGenerator(type)) {
                out_.print(F("OK switched to "));
                out_.println(pipeline_->getGeneratorName());
            } else {
                out_.println(F("ERROR: Failed to switch generator"));
            }
        } else {
            out_.print(F("Unknown generator: "));
            out_.println(name);
            out_.println(F("Use: fire, water, lightning, audio"));
        }
        return true;
    }

    return false;
}

// === EFFECT COMMANDS ===
bool SerialConsole::handleEffectCommand(const char* cmd) {
    if (!pipeline_) return false;

    // "effect list" - list available effects
    if (strcmp(cmd, "effect list") == 0 || strcmp(cmd, "effect") == 0) {
        out_.println(F("Available effects:"));
        for (int i = 0; i < RenderPipeline::NUM_EFFECTS; i++) {
            const char* name = RenderPipeline::getEffectNameByIndex(i);
            bool active = (RenderPipeline::getEffectTypeByIndex(i) == pipeline_->getEffectType());
            out_.print(F("  "));
            out_.print(name);
            if (active) out_.print(F(" (active)"));
            out_.println();
        }
        return true;
    }

    // "effect <name>" - switch to effect (or disable with "none")
    if (strncmp(cmd, "effect ", 7) == 0) {
        const char* name = cmd + 7;

        // Match effect by name
        EffectType type = EffectType::NONE;
        bool found = false;

        if (strcmp(name, "none") == 0 || strcmp(name, "off") == 0) {
            type = EffectType::NONE;
            found = true;
        } else if (strcmp(name, "hue") == 0 || strcmp(name, "huerotation") == 0) {
            type = EffectType::HUE_ROTATION;
            found = true;
        }

        if (found) {
            if (pipeline_->setEffect(type)) {
                out_.print(F("OK effect: "));
                out_.println(pipeline_->getEffectName());
            } else {
                out_.println(F("ERROR: Failed to set effect"));
            }
        } else {
            out_.print(F("Unknown effect: "));
            out_.println(name);
            out_.println(F("Use: none, hue"));
        }
        return true;
    }

    return false;
}

// === WATER SETTINGS (Particle-based) ===
// Prefixed with "w_" to avoid name collisions with fire settings.
// Pool auto-sized in begin(): capacity = maxParticles * numLeds.
void SerialConsole::registerWaterSettings(WaterParams* wp) {
    if (!wp) return;

    // Spawn behavior
    settings_.registerFloat("w_spawnchance", &wp->baseSpawnChance, "water",
        "Baseline drop spawn probability", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("w_audioboost", &wp->audioSpawnBoost, "water",
        "Audio reactivity multiplier", 0.0f, 2.0f, onParamChanged);

    // Physics (fractions × device dimensions)
    settings_.registerFloat("w_gravity", &wp->gravity, "water",
        "Gravity (x traversalDim -> LEDs/sec^2)", 0.0f, 5.0f, onParamChanged);
    settings_.registerFloat("w_windbase", &wp->windBase, "water",
        "Base wind force", -5.0f, 5.0f, onParamChanged);
    settings_.registerFloat("w_windvar", &wp->windVariation, "water",
        "Wind variation (x crossDim -> LEDs/sec)", 0.0f, 2.0f, onParamChanged);
    settings_.registerFloat("w_drag", &wp->drag, "water",
        "Drag coefficient", 0.9f, 1.0f, onParamChanged);

    // Drop appearance (fractions × device dimensions)
    settings_.registerFloat("w_dropvelmin", &wp->dropVelocityMin, "water",
        "Min velocity (x traversalDim -> LEDs/sec)", 0.0f, 2.0f, onParamChanged);
    settings_.registerFloat("w_dropvelmax", &wp->dropVelocityMax, "water",
        "Max velocity (x traversalDim -> LEDs/sec)", 0.0f, 2.0f, onParamChanged);
    settings_.registerFloat("w_dropspread", &wp->dropSpread, "water",
        "Spread (x crossDim -> LEDs/sec)", 0.0f, 5.0f, onParamChanged);

    // Splash behavior (fractions × device dimensions)
    settings_.registerFloat("w_splashparts", &wp->splashParticles, "water",
        "Splash particles (x crossDim -> count)", 0.0f, 5.0f, onParamChanged);
    settings_.registerFloat("w_splashvelmin", &wp->splashVelocityMin, "water",
        "Splash vel min (x traversalDim -> LEDs/sec)", 0.0f, 2.0f, onParamChanged);
    settings_.registerFloat("w_splashvelmax", &wp->splashVelocityMax, "water",
        "Splash vel max (x traversalDim -> LEDs/sec)", 0.0f, 2.0f, onParamChanged);
    settings_.registerUint8("w_splashint", &wp->splashIntensity, "water",
        "Splash particle intensity", 0, 255, onParamChanged);

    // Lifecycle
    settings_.registerFloat("w_maxparts", &wp->maxParticles, "water",
        "Max particles (× numLeds, clamped to pool)", 0.1f, 1.0f, onParamChanged);
    settings_.registerUint8("w_lifespan", &wp->defaultLifespan, "water",
        "Default particle lifespan (frames)", 20, 180, onParamChanged);
    settings_.registerUint8("w_intmin", &wp->intensityMin, "water",
        "Minimum spawn intensity", 0, 255, onParamChanged);
    settings_.registerUint8("w_intmax", &wp->intensityMax, "water",
        "Maximum spawn intensity", 0, 255, onParamChanged);

    // Audio reactivity
    settings_.registerFloat("w_musicpulse", &wp->musicSpawnPulse, "water",
        "Phase modulation for spawn rate", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("w_transmin", &wp->organicTransientMin, "water",
        "Min transient to trigger burst", 0.0f, 1.0f, onParamChanged);

    // Background
    settings_.registerFloat("w_bgintensity", &wp->backgroundIntensity, "water",
        "Noise background brightness", 0.0f, 1.0f, onParamChanged);
}

// === LIGHTNING SETTINGS (Particle-based) ===
// Prefixed with "l_" to avoid name collisions with fire settings.
// Pool auto-sized in begin(): capacity = maxParticles * numLeds.
void SerialConsole::registerLightningSettings(LightningParams* lp) {
    if (!lp) return;

    // Spawn behavior
    settings_.registerFloat("l_spawnchance", &lp->baseSpawnChance, "lightning",
        "Baseline bolt spawn probability", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("l_audioboost", &lp->audioSpawnBoost, "lightning",
        "Audio reactivity multiplier", 0.0f, 2.0f, onParamChanged);

    // Bolt appearance
    settings_.registerUint8("l_faderate", &lp->fadeRate, "lightning",
        "Intensity decay per frame", 0, 255, onParamChanged);

    // Branching behavior
    settings_.registerUint8("l_branchchance", &lp->branchChance, "lightning",
        "Branch probability (%)", 0, 100, onParamChanged);
    settings_.registerUint8("l_branchcount", &lp->branchCount, "lightning",
        "Branches per trigger", 1, 4, onParamChanged);
    settings_.registerFloat("l_branchspread", &lp->branchAngleSpread, "lightning",
        "Branch angle spread (radians)", 0.0f, 3.14159f, onParamChanged);
    settings_.registerUint8("l_branchintloss", &lp->branchIntensityLoss, "lightning",
        "Branch intensity reduction (%)", 0, 100, onParamChanged);

    // Lifecycle
    settings_.registerFloat("l_maxparts", &lp->maxParticles, "lightning",
        "Max particles (× numLeds, clamped to pool)", 0.1f, 1.0f, onParamChanged);
    settings_.registerUint8("l_lifespan", &lp->defaultLifespan, "lightning",
        "Default particle lifespan (frames)", 10, 60, onParamChanged);
    settings_.registerUint8("l_intmin", &lp->intensityMin, "lightning",
        "Minimum spawn intensity", 0, 255, onParamChanged);
    settings_.registerUint8("l_intmax", &lp->intensityMax, "lightning",
        "Maximum spawn intensity", 0, 255, onParamChanged);

    // Audio reactivity
    settings_.registerFloat("l_musicpulse", &lp->musicSpawnPulse, "lightning",
        "Phase modulation for spawn rate", 0.0f, 1.0f, onParamChanged);
    settings_.registerFloat("l_transmin", &lp->organicTransientMin, "lightning",
        "Min transient to trigger burst", 0.0f, 1.0f, onParamChanged);

    // Background
    settings_.registerFloat("l_bgintensity", &lp->backgroundIntensity, "lightning",
        "Noise background brightness", 0.0f, 1.0f, onParamChanged);
}

// === AUDIO VISUALIZATION GENERATOR SETTINGS ===
void SerialConsole::registerAudioVisSettings(AudioParams* ap) {
    if (!ap) return;

    // Transient visualization (green gradient from top)
    settings_.registerFloat("transientrowfrac", &ap->transientRowFraction, "audiovis",
        "Fraction of height for transient indicator", 0.1f, 0.5f, onParamChanged);
    settings_.registerFloat("transientdecay", &ap->transientDecayRate, "audiovis",
        "Transient decay rate per frame", 0.01f, 0.5f, onParamChanged);
    settings_.registerUint8("transientbright", &ap->transientBrightness, "audiovis",
        "Maximum transient brightness", 0, 255, onParamChanged);

    // Energy level visualization (yellow row)
    settings_.registerUint8("levelbright", &ap->levelBrightness, "audiovis",
        "Energy level row brightness", 0, 255, onParamChanged);
    settings_.registerFloat("levelsmooth", &ap->levelSmoothing, "audiovis",
        "Energy level smoothing factor", 0.0f, 0.99f, onParamChanged);

    // Phase visualization (blue row, full height)
    settings_.registerUint8("phasebright", &ap->phaseBrightness, "audiovis",
        "Phase row maximum brightness", 0, 255, onParamChanged);
    settings_.registerFloat("musicmodethresh", &ap->musicModeThreshold, "audiovis",
        "Rhythm confidence threshold for phase display", 0.0f, 1.0f, onParamChanged);

    // Beat pulse (blue center band on beat)
    settings_.registerUint8("beatpulsebright", &ap->beatPulseBrightness, "audiovis",
        "Beat pulse band max brightness", 0, 255, onParamChanged);
    settings_.registerFloat("beatpulsedecay", &ap->beatPulseDecay, "audiovis",
        "Beat pulse decay rate per frame", 0.01f, 0.5f, onParamChanged);
    settings_.registerFloat("beatpulsewidth", &ap->beatPulseWidth, "audiovis",
        "Beat pulse band width as fraction of height", 0.05f, 0.5f, onParamChanged);

    // Background
    settings_.registerUint8("bgbright", &ap->backgroundBrightness, "audiovis",
        "Background brightness", 0, 255, onParamChanged);
}

// === EFFECT SETTINGS ===
void SerialConsole::registerEffectSettings() {
    if (!hueEffect_) return;

    // Initialize file-scope statics from current effect state
    effectHueShift_ = hueEffect_->getHueShift();
    effectRotationSpeed_ = hueEffect_->getRotationSpeed();

    settings_.registerFloat("hueshift", &effectHueShift_, "effect",
        "Static hue offset (0-1)", 0.0f, 1.0f);
    settings_.registerFloat("huespeed", &effectRotationSpeed_, "effect",
        "Auto-rotation speed (cycles/sec)", 0.0f, 2.0f);
}

void SerialConsole::syncEffectSettings() {
    if (!hueEffect_) return;

    // Apply file-scope statics (modified by SettingsRegistry) to the actual effect
    hueEffect_->setHueShift(effectHueShift_);
    hueEffect_->setRotationSpeed(effectRotationSpeed_);
}

void SerialConsole::streamTick() {
    if (!streamEnabled_ && !streamNN_) return;

    uint32_t now = millis();

    // NN diagnostic stream: fires every spectral frame (~62.5 Hz)
    // Outputs the exact mel bands fed to the NN + NN output for offline validation.
    // Format: {"type":"NN","ts":<ms>,"mel":[26 floats],"onset":<float>,"nn":<0|1>,"bpm":<float>,"phase":<float>,"rstr":<float>,"lvl":<float>,"gain":<float>}
    // "onset" = raw ODF (NN onset activation or mic level fallback)
    // "nn" = 1 if NN loaded, 0 if stub/fallback
    // "bpm" = current estimated tempo
    if (streamNN_ && audioCtrl_) {
        const SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
        uint32_t fc = spectral.getFrameCount();
        if (fc != lastNNFrameCount_) {
            lastNNFrameCount_ = fc;
            const float* mel = spectral.getRawMelBands();

            out_.print(F("{\"type\":\"NN\",\"ts\":"));
            out_.print(now);
            out_.print(F(",\"mel\":["));
            for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
                if (i > 0) out_.print(',');
                out_.print(mel[i], 4);
            }
            // onset = last pulse strength (NN activation or mic level fallback)
            out_.print(F("],\"onset\":"));
            out_.print(audioCtrl_->getLastOnsetStrength(), 4);
            // Note (v65): "nn" field is now always present in both NN and non-NN builds.
            // Previously it was ifdef-guarded; non-NN builds emitted "nn":0 via #else.
            // The stub's isReady() returns false, so the value is still 0 in non-NN builds.
            out_.print(F(",\"nn\":"));
            out_.print(audioCtrl_->getFrameOnsetNN().isReady() ? 1 : 0);
            out_.print(F(",\"bpm\":"));
            out_.print(audioCtrl_->getCurrentBpm(), 1);
            out_.print(F(",\"phase\":"));
            out_.print(audioCtrl_->getControl().phase, 4);
            out_.print(F(",\"rstr\":"));
            out_.print(audioCtrl_->getControl().rhythmStrength, 3);
            out_.print(F(",\"lvl\":"));
            out_.print(mic_->getLevel(), 3);
            out_.print(F(",\"gain\":"));
            out_.print(mic_->getHwGain());
            out_.println(F("}"));
        }
    }

    // STATUS update at ~1Hz
    static uint32_t lastStatusMs = 0;
    if (mic_ && (now - lastStatusMs >= 1000)) {
        lastStatusMs = now;
        out_.print(F("{\"type\":\"STATUS\",\"ts\":"));
        out_.print(now);
        out_.print(F(",\"mode\":\"ensemble\""));
        out_.print(F(",\"hwGain\":"));
        out_.print(mic_->getHwGain());
        out_.print(F(",\"level\":"));
        out_.print(mic_->getLevel(), 2);
        out_.print(F(",\"peakLevel\":"));
        out_.print(mic_->getPeakLevel(), 2);
        out_.println(F("}"));
    }

    // Audio streaming at ~20Hz (normal) or ~100Hz (fast mode for testing)
    // Skip when NN-only stream is active (saves serial bandwidth for mel bands)
    uint16_t period = streamFast_ ? STREAM_FAST_PERIOD_MS : STREAM_PERIOD_MS;
    if (streamEnabled_ && mic_ && (now - streamLastMs_ >= period)) {
        streamLastMs_ = now;

        // Output compact JSON for web app (abbreviated field names for serial bandwidth)
        // Format: {"a":{"l":0.45,"t":0.85,"pk":0.32,"vl":0.04,"raw":0.12,"h":32,"alive":1,"z":0.15}}
        //
        // Field Mapping (abbreviated → full name : range):
        // l     → level            : 0-1 (post-range-mapping output, noise-gated)
        // t     → transient        : 0-1 (ensemble transient strength from all detectors)
        // pk    → peak             : 0-1 (current tracked peak for window normalization, raw range)
        // vl    → valley           : 0-1 (current tracked valley for window normalization, raw range)
        // raw   → raw ADC level    : 0-1 (what HW gain targets, pre-normalization)
        // h     → hardware gain    : 0-80 (PDM gain setting)
        // alive → PDM alive status : 0 or 1 (microphone health: 0=dead, 1=working)
        // z     → zero-crossing    : 0-1 (zero-crossing rate, for frequency classification)
        //
        // Debug mode additional fields:
        // agree → detector agreement : 0-7 (how many detectors fired)
        // conf  → ensemble confidence: 0-1 (combined confidence score)
        out_.print(F("{\"a\":{\"l\":"));
        out_.print(mic_->getLevel(), 2);
        out_.print(F(",\"t\":"));
        // Pulse strength from ODF-derived pulse detection (v67)
        float transient = 0.0f;
        if (audioCtrl_) {
            transient = audioCtrl_->getLastPulseStrength();
        }
        out_.print(transient, 2);
        out_.print(F(",\"pk\":"));
        out_.print(mic_->getPeakLevel(), 2);
        out_.print(F(",\"vl\":"));
        out_.print(mic_->getValleyLevel(), 2);
        out_.print(F(",\"raw\":"));
        out_.print(mic_->getRawLevel(), 2);
        out_.print(F(",\"h\":"));
        out_.print(mic_->getHwGain());
        out_.print(F(",\"alive\":"));
        out_.print(mic_->isPdmAlive() ? 1 : 0);

        // Debug mode: add pulse and spectral state
        // (BandFlux per-band flux fields removed v67 — BandFlux pipeline removed)
        if (streamDebug_ && audioCtrl_) {
            out_.print(F(",\"pulse\":"));
            out_.print(audioCtrl_->getLastPulseStrength(), 3);

            // Spectral processing state (compressor + whitening)
            const SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
            out_.print(F(",\"rms\":"));
            out_.print(spectral.getFrameRmsDb(), 1);
            out_.print(F(",\"cg\":"));
            out_.print(spectral.getSmoothedGainDb(), 2);
        }

        out_.print(F("}"));

        // AudioTracker music stream (v79 — PLP architecture)
        // Format: "m":{"a":1,"bpm":125.3,"ph":0.45,"pp":0.82,"str":0.72,"q":0,"e":0.5,"p":0.8,"od":3.2}
        // a = rhythm active, bpm = tempo, ph = PLP phase (0-1)
        // pp = PLP pulse (extracted pattern value), str = rhythm strength
        // q = beat event (phase wrap), e = energy, p = pulse (transient), od = onset density
        // Debug adds: conf = ACF periodicity, sl = slot cache {id, conf[]}
        if (audioCtrl_) {
            const AudioControl& audio = audioCtrl_->getControl();

            // Detect beat events via phase wrapping (>0.8 → <0.2)
            static float lastStreamPhase = 0.0f;
            float currentPhase = audio.phase;
            int beatEvent = (lastStreamPhase > 0.8f && currentPhase < 0.2f && audio.rhythmStrength > audioCtrl_->activationThreshold) ? 1 : 0;
            lastStreamPhase = currentPhase;

            out_.print(F(",\"m\":{\"a\":"));
            out_.print(audio.rhythmStrength > audioCtrl_->activationThreshold ? 1 : 0);
            out_.print(F(",\"bpm\":"));
            out_.print(audioCtrl_->getCurrentBpm(), 1);
            out_.print(F(",\"ph\":"));
            out_.print(currentPhase, 2);
            out_.print(F(",\"pp\":"));
            out_.print(audio.plpPulse, 3);
            out_.print(F(",\"str\":"));
            out_.print(audio.rhythmStrength, 2);
            out_.print(F(",\"q\":"));
            out_.print(beatEvent);
            out_.print(F(",\"e\":"));
            out_.print(audio.energy, 2);
            out_.print(F(",\"p\":"));
            out_.print(audio.pulse, 2);
            out_.print(F(",\"od\":"));
            out_.print(audioCtrl_->getOnsetDensity(), 1);
            out_.print(F(",\"nn\":"));
            out_.print(audioCtrl_->getRawNNActivation(), 3);

            // Debug mode: add diagnostics
            if (streamDebug_) {
                out_.print(F(",\"conf\":"));
                out_.print(audioCtrl_->getPeriodicityStrength(), 3);
                out_.print(F(",\"sl\":{\"id\":"));
                out_.print(audioCtrl_->getActiveSlotId());
                out_.print(F(",\"conf\":["));
                for (int si = 0; si < audioCtrl_->getSlotCount(); si++) {
                    if (si > 0) out_.print(F(","));
                    const PatternSlot& slot = audioCtrl_->getSlot(si);
                    out_.print(slot.valid ? slot.confidence : 0.0f, 2);
                }
                out_.print(F("]}"));
            }

            out_.print(F("}"));
        }

        // LED brightness telemetry
        // NOTE: Particle-based generators don't track total heat/brightness
        // in the same way, so these stats are not available
        // TODO: Add particle pool statistics (active count, etc.)

        out_.println(F("}"));
    }

    // Battery streaming at ~1Hz
    if (battery_ && (now - batteryLastMs_ >= BATTERY_PERIOD_MS)) {
        batteryLastMs_ = now;

        // Output battery status JSON
        // Format: {"b":{"n":true,"c":false,"v":3.85,"p":72}}
        // n = connected (battery detected)
        // c = charging (true if charging)
        // v = voltage (in volts)
        // p = percent (0-100)
        out_.print(F("{\"b\":{\"n\":"));
        out_.print(battery_->isBatteryConnected() ? F("true") : F("false"));
        out_.print(F(",\"c\":"));
        out_.print(battery_->isCharging() ? F("true") : F("false"));
        out_.print(F(",\"v\":"));
        out_.print(battery_->getVoltage(), 2);
        out_.print(F(",\"p\":"));
        out_.print(battery_->getPercent());
        out_.println(F("}}"));
    }
}

// (handleEnsembleCommand removed v67 — BandFlux pipeline removed)
// "show nn" moved to handleBeatTrackingCommand
// "pulsenear"/"pulsefar" commands moved to handleBeatTrackingCommand
// "show detectors"/"show ensemble"/ensemble_*/detector_* commands deleted

// === LOG LEVEL COMMANDS ===
bool SerialConsole::handleLogCommand(const char* cmd) {
    // "log" - show current level
    if (strcmp(cmd, "log") == 0) {
        out_.print(F("Log level: "));
        switch (logLevel_) {
            case LogLevel::OFF:   out_.println(F("off")); break;
            case LogLevel::ERROR: out_.println(F("error")); break;
            case LogLevel::WARN:  out_.println(F("warn")); break;
            case LogLevel::INFO:  out_.println(F("info")); break;
            case LogLevel::DEBUG: out_.println(F("debug")); break;
        }
        return true;
    }

    // "log off" - disable logging
    if (strcmp(cmd, "log off") == 0) {
        logLevel_ = LogLevel::OFF;
        out_.println(F("OK log off"));
        return true;
    }

    // "log error" - errors only
    if (strcmp(cmd, "log error") == 0) {
        logLevel_ = LogLevel::ERROR;
        out_.println(F("OK log error"));
        return true;
    }

    // "log warn" - warnings and errors
    if (strcmp(cmd, "log warn") == 0) {
        logLevel_ = LogLevel::WARN;
        out_.println(F("OK log warn"));
        return true;
    }

    // "log info" - info and above (default)
    if (strcmp(cmd, "log info") == 0) {
        logLevel_ = LogLevel::INFO;
        out_.println(F("OK log info"));
        return true;
    }

    // "log debug" - all messages
    if (strcmp(cmd, "log debug") == 0) {
        logLevel_ = LogLevel::DEBUG;
        out_.println(F("OK log debug"));
        return true;
    }

    return false;
}

// === DEBUG CHANNEL COMMANDS ===
// Controls per-subsystem JSON debug output independently from log levels
bool SerialConsole::handleDebugCommand(const char* cmd) {
    // "debug" - show enabled channels
    if (strcmp(cmd, "debug") == 0) {
        out_.println(F("Debug channels:"));
        out_.print(F("  transient:  ")); out_.println(isDebugChannelEnabled(DebugChannel::TRANSIENT) ? F("ON") : F("off"));
        out_.print(F("  rhythm:     ")); out_.println(isDebugChannelEnabled(DebugChannel::RHYTHM) ? F("ON") : F("off"));
        out_.print(F("  audio:      ")); out_.println(isDebugChannelEnabled(DebugChannel::AUDIO) ? F("ON") : F("off"));
        out_.print(F("  generator:  ")); out_.println(isDebugChannelEnabled(DebugChannel::GENERATOR) ? F("ON") : F("off"));
        out_.print(F("  ensemble:   ")); out_.println(isDebugChannelEnabled(DebugChannel::ENSEMBLE) ? F("ON") : F("off"));
        return true;
    }

    // Helper lambda to parse channel name
    auto parseChannel = [](const char* name) -> DebugChannel {
        if (strcmp(name, "transient") == 0)  return DebugChannel::TRANSIENT;
        if (strcmp(name, "rhythm") == 0)     return DebugChannel::RHYTHM;
        if (strcmp(name, "audio") == 0)      return DebugChannel::AUDIO;
        if (strcmp(name, "generator") == 0)  return DebugChannel::GENERATOR;
        if (strcmp(name, "ensemble") == 0)   return DebugChannel::ENSEMBLE;
        if (strcmp(name, "all") == 0)        return DebugChannel::ALL;
        return DebugChannel::NONE;
    };

    // "debug <channel> on" or "debug <channel> off"
    // Also handles "debug all on/off" via parseChannel returning ALL
    if (strncmp(cmd, "debug ", 6) == 0) {
        const char* rest = cmd + 6;
        char channelName[16] = {0};
        const char* space = strchr(rest, ' ');

        if (space && static_cast<size_t>(space - rest) < sizeof(channelName)) {
            strncpy(channelName, rest, space - rest);
            channelName[space - rest] = '\0';

            DebugChannel channel = parseChannel(channelName);
            if (channel == DebugChannel::NONE) {
                out_.print(F("Unknown channel: "));
                out_.println(channelName);
                out_.println(F("Valid: transient, rhythm, audio, generator, ensemble, all"));
                return true;
            }

            const char* action = space + 1;
            if (strcmp(action, "on") == 0) {
                enableDebugChannel(channel);
                out_.print(F("OK debug "));
                out_.print(channelName);
                out_.println(F(" on"));
                return true;
            } else if (strcmp(action, "off") == 0) {
                disableDebugChannel(channel);
                out_.print(F("OK debug "));
                out_.print(channelName);
                out_.println(F(" off"));
                return true;
            } else {
                out_.print(F("Invalid action: "));
                out_.println(action);
                out_.println(F("Use 'on' or 'off'"));
                return true;
            }
        }

        out_.println(F("Usage: debug <channel> on|off"));
        out_.println(F("Channels: transient, rhythm, audio, generator, ensemble, all"));
        return true;
    }

    return false;
}

// === FAKE AUDIO COMMANDS ===
// Synthetic 120 BPM 4/4 dance pattern for visual design and debugging.
// Overrides real audio in renderFrame() when enabled.
bool SerialConsole::handleFakeAudioCommand(const char* cmd) {
    if (strncmp(cmd, "fakeaudio", 9) != 0) return false;

    const char* arg = cmd + 9;
    while (*arg == ' ') arg++;  // skip spaces

    if (*arg == '\0') {
        // "fakeaudio" — show current state
        out_.print(F("fakeaudio: "));
        out_.println(fakeAudio_ && fakeAudio_->isEnabled() ? F("ON") : F("off"));
        out_.println(F("Usage: fakeaudio on|off"));
        return true;
    }

    if (!fakeAudio_) {
        out_.println(F("ERROR: FakeAudio not available"));
        return true;
    }

    if (strcmp(arg, "on") == 0) {
        fakeAudio_->enable();
        out_.println(F("OK fakeaudio ON — 120 BPM 4/4 synthetic pattern active"));
        return true;
    }

    if (strcmp(arg, "off") == 0) {
        fakeAudio_->disable();
        out_.println(F("OK fakeaudio off"));
        return true;
    }

    out_.println(F("Usage: fakeaudio on|off"));
    return true;
}

// === BLE DIAGNOSTICS ===
bool SerialConsole::handleBleCommand(const char* cmd) {
    if (strncmp(cmd, "ble", 3) != 0) return false;

    const char* arg = cmd + 3;
    while (*arg == ' ') arg++;

#ifdef BLINKY_PLATFORM_NRF52840
    if (*arg == '\0') {
        // "ble" — show NUS and scanner status
        if (bleNus_) {
            bleNus_->printDiagnostics();
        }
        if (bleScanner_) {
            bleScanner_->printDiagnostics();
        }
        if (!bleNus_ && !bleScanner_) {
            out_.println(F("[BLE] Not initialized"));
        }
        return true;
    }

    if (strcmp(arg, "nus") == 0) {
        if (bleNus_) {
            bleNus_->printDiagnostics();
        } else {
            out_.println(F("[BLE] NUS not initialized"));
        }
        return true;
    }

    if (strcmp(arg, "scan") == 0) {
        // "ble scan" — show what the scanner has seen
        if (bleScanner_) {
            out_.println(F("[BLE] Passive scan active (matching packets shown via 'ble')"));
            out_.print(F("[BLE] Total received: "));
            out_.println(bleScanner_->getPacketsReceived());
            if (bleScanner_->getPacketsReceived() > 0) {  // cppcheck-suppress knownConditionTrueFalse
                out_.print(F("[BLE] Last RSSI: "));
                out_.print(bleScanner_->getLastRssi());
                out_.println(F("dBm"));
            }
        } else {
            out_.println(F("[BLE] Scanner not initialized"));
        }
        return true;
    }
#elif defined(BLINKY_PLATFORM_ESP32S3)
    if (*arg == '\0') {
        // "ble" — show advertiser status
        if (bleAdvertiser_) {
            bleAdvertiser_->printDiagnostics();
        } else {
            out_.println(F("[BLE] Advertiser not initialized"));
        }
        return true;
    }

    if (strcmp(arg, "status") == 0) {
        if (bleAdvertiser_) {
            bleAdvertiser_->printDiagnostics();
        } else {
            out_.println(F("[BLE] Advertiser not initialized"));
        }
        return true;
    }

    if (strncmp(arg, "broadcast ", 10) == 0) {
        // "ble broadcast <payload>" — send a command to fleet
        const char* payload = arg + 10;
        while (*payload == ' ') payload++;
        if (*payload == '\0') {
            out_.println(F("Usage: ble broadcast <json_or_command>"));
            return true;
        }
        if (!bleAdvertiser_) {
            out_.println(F("[BLE] Advertiser not initialized"));
            return true;
        }
        bool ok = bleAdvertiser_->broadcastCommand(payload);
        if (ok) {
            out_.print(F("[BLE] Broadcast sent: "));
            out_.println(payload);
        } else {
            out_.println(F("[BLE] Broadcast failed"));
        }
        return true;
    }
#else
    if (*arg == '\0') {
        out_.println(F("[BLE] Not available on this platform"));
        return true;
    }
#endif

    out_.println(F("Usage: ble [scan|status|broadcast <payload>]"));
    return true;
}

// === WIFI COMMANDS (ESP32-S3 only) ===
bool SerialConsole::handleWifiCommand(const char* cmd) {
    if (strncmp(cmd, "wifi", 4) != 0) return false;

    const char* arg = cmd + 4;
    while (*arg == ' ') arg++;

#ifdef BLINKY_PLATFORM_ESP32S3
    if (*arg == '\0') {
        // "wifi" — show status
        if (wifiManager_) {
            wifiManager_->printStatus();
        } else {
            out_.println(F("[WiFi] Not initialized"));
        }
        return true;
    }

    if (strcmp(arg, "status") == 0) {
        if (wifiManager_) {
            wifiManager_->printStatus();
        } else {
            out_.println(F("[WiFi] Not initialized"));
        }
        return true;
    }

    if (strncmp(arg, "ssid ", 5) == 0) {
        const char* ssid = arg + 5;
        while (*ssid == ' ') ssid++;
        if (*ssid == '\0') {
            out_.println(F("Usage: wifi ssid <name>"));
            return true;
        }
        if (wifiManager_) {
            wifiManager_->setSsid(ssid);
        }
        return true;
    }

    if (strncmp(arg, "pass ", 5) == 0) {
        const char* pass = arg + 5;
        while (*pass == ' ') pass++;
        if (*pass == '\0') {
            out_.println(F("Usage: wifi pass <key>"));
            return true;
        }
        if (wifiManager_) {
            wifiManager_->setPassword(pass);
        }
        return true;
    }

    if (strcmp(arg, "connect") == 0) {
        if (wifiManager_) {
            wifiManager_->connect();
        }
        return true;
    }

    if (strcmp(arg, "disconnect") == 0) {
        if (wifiManager_) {
            wifiManager_->disconnect();
        }
        return true;
    }

    if (strcmp(arg, "clear") == 0) {
        if (wifiManager_) {
            wifiManager_->clearCredentials();
        }
        return true;
    }

    out_.println(F("Usage: wifi [ssid <name>|pass <key>|connect|disconnect|clear|status]"));
    return true;
#else
    // WiFi not available on nRF52840
    out_.println(F("[WiFi] Not available on this platform"));
    return true;
#endif
}

// === TRACKER COMMANDS (AudioTracker) ===
bool SerialConsole::handleBeatTrackingCommand(const char* cmd) {
    if (!audioCtrl_) return false;

    // "show nn" - NN diagnostics
    if (strcmp(cmd, "show nn") == 0) {
        audioCtrl_->getFrameOnsetNN().printDiagnostics();
        return true;
    }

    // "show beat" - tracker state
    if (strcmp(cmd, "show beat") == 0) {
        out_.println(F("=== AudioTracker (ACF+PLP) ==="));
        out_.print(F("BPM: "));
        out_.println(audioCtrl_->getCurrentBpm(), 1);
        out_.print(F("Phase: "));
        out_.println(audioCtrl_->getPlpPhase(), 3);
        out_.print(F("Periodicity: "));
        out_.println(audioCtrl_->getPeriodicityStrength(), 3);
        out_.print(F("Beat Count: "));
        out_.println(audioCtrl_->getBeatCount());
        out_.print(F("PLP Confidence: "));
        out_.println(audioCtrl_->getPlpConfidence(), 4);
        out_.print(F("PLP Pulse: "));
        out_.println(audioCtrl_->getPlpPulseValue(), 3);
        out_.print(F("PLP DFT Mag: "));
        out_.println(audioCtrl_->getPlpDftMag(), 3);
        { const char* srcNames[] = {"flux", "bass", "nn"};
          out_.print(F("PLP Source: "));
          out_.println(srcNames[audioCtrl_->getPlpBestSource()]); }
        out_.print(F("Beat Stability: "));
        out_.println(audioCtrl_->getBeatStability(), 3);
        out_.print(F("Pulse: "));
        out_.println(audioCtrl_->getLastPulseStrength(), 3);
        out_.print(F("Onset Density: "));
        out_.print(audioCtrl_->getControl().onsetDensity, 1);
        out_.println(F(" /s"));
        out_.println();
        return true;
    }

    // "json rhythm" / "json beat" - JSON output for test automation
    if (strcmp(cmd, "json rhythm") == 0 || strcmp(cmd, "json beat") == 0) {
        out_.print(F("{\"bpm\":"));
        out_.print(audioCtrl_->getCurrentBpm(), 1);
        out_.print(F(",\"phase\":"));
        out_.print(audioCtrl_->getPlpPhase(), 3);
        out_.print(F(",\"periodicity\":"));
        out_.print(audioCtrl_->getPeriodicityStrength(), 3);
        out_.print(F(",\"beatCount\":"));
        out_.print(audioCtrl_->getBeatCount());
        out_.print(F(",\"rhythmStrength\":"));
        out_.print(audioCtrl_->getControl().rhythmStrength, 3);
        out_.print(F(",\"pulse\":"));
        out_.print(audioCtrl_->getLastPulseStrength(), 3);
        out_.print(F(",\"plpConf\":"));
        out_.print(audioCtrl_->getPlpConfidence(), 3);
        out_.print(F(",\"plpPulse\":"));
        out_.print(audioCtrl_->getPlpPulseValue(), 3);
        out_.print(F(",\"onsetDensity\":"));
        out_.print(audioCtrl_->getControl().onsetDensity, 1);
        out_.println(F("}"));
        return true;
    }

    // "json pattern" / "json slots" - compact JSON for test automation (v82)
    if (strcmp(cmd, "json pattern") == 0 || strcmp(cmd, "json slots") == 0) {
        out_.print(F("{\"active\":"));
        out_.print(audioCtrl_->getActiveSlotId());
        out_.print(F(",\"slots\":["));
        for (int i = 0; i < audioCtrl_->getSlotCount(); i++) {
            if (i > 0) out_.print(F(","));
            const PatternSlot& slot = audioCtrl_->getSlot(i);
            out_.print(F("{\"conf\":"));
            out_.print(slot.confidence, 3);
            out_.print(F(",\"bars\":"));
            out_.print(slot.totalBars);
            out_.print(F(",\"valid\":"));
            out_.print(slot.valid ? F("true") : F("false"));
            out_.print(F(",\"bb\":["));
            for (int j = 0; j < SLOT_BINS; j++) {
                if (j > 0) out_.print(F(","));
                out_.print(slot.bins[j], 3);
            }
            out_.print(F("]}"));
        }
        out_.println(F("]}"));
        return true;
    }

    // "reset pattern" / "reset slots" - zero all slot cache state (for test automation)
    if (strcmp(cmd, "reset pattern") == 0 || strcmp(cmd, "reset slots") == 0) {
        audioCtrl_->resetSlots();
        out_.println(F("OK"));
        return true;
    }

    // "show pattern" / "show slots" - pattern slot cache state (v82)
    if (strcmp(cmd, "show pattern") == 0 || strcmp(cmd, "show slots") == 0) {
        out_.println(F("=== Pattern Slot Cache ==="));
        out_.print(F("  Active Slot: "));
        out_.println(audioCtrl_->getActiveSlotId());
        out_.print(F("  PLP Confidence: "));
        out_.println(audioCtrl_->getPlpConfidence(), 3);
        out_.print(F("  BPM: "));
        out_.println(audioCtrl_->getCurrentBpm(), 1);
        for (int i = 0; i < audioCtrl_->getSlotCount(); i++) {
            const PatternSlot& slot = audioCtrl_->getSlot(i);
            out_.print(F("  Slot "));
            out_.print(i);
            out_.print(F(": "));
            if (!slot.valid) {
                out_.println(F("[empty]"));
                continue;
            }
            out_.print(F("conf="));
            out_.print(slot.confidence, 3);
            out_.print(F(" bars="));
            out_.print(slot.totalBars);
            out_.print(F(" age="));
            out_.print(slot.age);
            if (i == audioCtrl_->getActiveSlotId()) out_.print(F(" *ACTIVE*"));
            out_.print(F("\n    bins=["));
            for (int j = 0; j < SLOT_BINS; j++) {
                if (j > 0) out_.print(F(","));
                out_.print(slot.bins[j], 2);
            }
            out_.println(F("]"));
        }
        out_.println();
        return true;
    }

    // "show spectral" - spectral processing state
    if (strcmp(cmd, "show spectral") == 0) {
        const SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
        out_.println(F("=== Spectral Processing ==="));
        out_.println(F("-- Compressor --"));
        out_.print(F("  Enabled: ")); out_.println(spectral.compressorEnabled ? "yes" : "no");
        out_.print(F("  Threshold: ")); out_.print(spectral.compThresholdDb, 1); out_.println(F(" dB"));
        out_.print(F("  Ratio: ")); out_.print(spectral.compRatio, 1); out_.println(F(":1"));
        out_.print(F("  Knee: ")); out_.print(spectral.compKneeDb, 1); out_.println(F(" dB"));
        out_.print(F("  Makeup: ")); out_.print(spectral.compMakeupDb, 1); out_.println(F(" dB"));
        out_.print(F("  Attack: ")); out_.print(spectral.compAttackTau * 1000.0f, 1); out_.println(F(" ms"));
        out_.print(F("  Release: ")); out_.print(spectral.compReleaseTau, 2); out_.println(F(" s"));
        out_.print(F("  Frame RMS: ")); out_.print(spectral.getFrameRmsDb(), 1); out_.println(F(" dB"));
        out_.print(F("  Smoothed Gain: ")); out_.print(spectral.getSmoothedGainDb(), 2); out_.println(F(" dB"));
        out_.println(F("-- Whitening --"));
        out_.print(F("  Enabled: ")); out_.println(spectral.whitenEnabled ? "yes" : "no");
        out_.print(F("  Decay: ")); out_.println(spectral.whitenDecay, 4);
        out_.print(F("  Floor: ")); out_.println(spectral.whitenFloor, 4);
        out_.println();
        return true;
    }

    // "json spectral" - spectral processing state as JSON (for test automation)
    if (strcmp(cmd, "json spectral") == 0) {
        const SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
        out_.print(F("{\"compEnabled\":"));
        out_.print(spectral.compressorEnabled ? 1 : 0);
        out_.print(F(",\"compThreshDb\":"));
        out_.print(spectral.compThresholdDb, 1);
        out_.print(F(",\"compRatio\":"));
        out_.print(spectral.compRatio, 1);
        out_.print(F(",\"compKneeDb\":"));
        out_.print(spectral.compKneeDb, 1);
        out_.print(F(",\"compMakeupDb\":"));
        out_.print(spectral.compMakeupDb, 1);
        out_.print(F(",\"compAttackMs\":"));
        out_.print(spectral.compAttackTau * 1000.0f, 2);
        out_.print(F(",\"compReleaseS\":"));
        out_.print(spectral.compReleaseTau, 2);
        out_.print(F(",\"rmsDb\":"));
        out_.print(spectral.getFrameRmsDb(), 1);
        out_.print(F(",\"gainDb\":"));
        out_.print(spectral.getSmoothedGainDb(), 2);
        out_.print(F(",\"whitenEnabled\":"));
        out_.print(spectral.whitenEnabled ? 1 : 0);
        out_.print(F(",\"whitenDecay\":"));
        out_.print(spectral.whitenDecay, 4);
        out_.print(F(",\"whitenFloor\":"));
        out_.print(spectral.whitenFloor, 4);
        out_.println(F("}"));
        return true;
    }

    return false;
}

// === LOGGING HELPERS ===
// These are static methods — use instance_->out_ when available, fall back to Serial.
void SerialConsole::logDebug(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::DEBUG) {
        Print& p = instance_ ? (Print&)instance_->out_ : Serial;
        p.print(F("[DEBUG] "));
        p.println(msg);
    }
}

void SerialConsole::logInfo(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::INFO) {
        Print& p = instance_ ? (Print&)instance_->out_ : Serial;
        p.print(F("[INFO] "));
        p.println(msg);
    }
}

void SerialConsole::logWarn(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::WARN) {
        Print& p = instance_ ? (Print&)instance_->out_ : Serial;
        p.print(F("[WARN] "));
        p.println(msg);
    }
}

void SerialConsole::logError(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::ERROR) {
        Print& p = instance_ ? (Print&)instance_->out_ : Serial;
        p.print(F("[ERROR] "));
        p.println(msg);
    }
}
