#include "SerialConsole.h"
#include "../hal/PlatformDetect.h"
#include "../types/BlinkyAssert.h"
#include "../config/TotemDefaults.h"
#include "AdaptiveMic.h"
#include "BatteryMonitor.h"
#include "../audio/AudioController.h"
#include "../devices/DeviceConfig.h"
#include "../config/ConfigStorage.h"
#include "../config/DeviceConfigLoader.h"  // v28: Runtime device config loading
#include "../types/Version.h"
#include "../render/RenderPipeline.h"
#include "../effects/HueRotationEffect.h"
#include <ArduinoJson.h>  // v28: JSON parsing for device config upload

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
    : pipeline_(pipeline), fireGenerator_(nullptr), waterGenerator_(nullptr),
      lightningGenerator_(nullptr), audioVisGenerator_(nullptr), hueEffect_(nullptr), mic_(mic),
      battery_(nullptr), audioCtrl_(nullptr), configStorage_(nullptr) {
    instance_ = this;
    // Get generator pointers from pipeline
    if (pipeline_) {
        fireGenerator_ = pipeline_->getFireGenerator();
        waterGenerator_ = pipeline_->getWaterGenerator();
        lightningGenerator_ = pipeline_->getLightningGenerator();
        audioVisGenerator_ = pipeline_->getAudioVisGenerator();
        hueEffect_ = pipeline_->getHueRotationEffect();
    }
}

void SerialConsole::begin() {
    // Note: Serial.begin() should be called by main setup() before this
    settings_.begin();
    registerSettings();

    Serial.println(F("Serial console ready."));
}

void SerialConsole::registerSettings() {
    // Get direct pointers to the fire generator's params
    FireParams* fp = nullptr;
    if (fireGenerator_) {
        fp = &fireGenerator_->getParamsMutable();
    }

    // Register all settings by category
    registerFireSettings(fp);

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
    registerRhythmSettings();
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

// === AUDIO SETTINGS ===
// Window/Range normalization: peak/valley tracking adapts to signal
void SerialConsole::registerAudioSettings() {
    if (!mic_) return;

    settings_.registerFloat("peaktau", &mic_->peakTau, "audio",
        "Peak adaptation speed (s)", 0.5f, 10.0f);
    settings_.registerFloat("releasetau", &mic_->releaseTau, "audio",
        "Peak release speed (s)", 1.0f, 30.0f);
}

// === HARDWARE AGC SETTINGS ===
// Signal flow: Mic → HW Gain (PRIMARY) → ADC → Window/Range (SECONDARY) → Output
void SerialConsole::registerAgcSettings() {
    if (!mic_) return;

    settings_.registerFloat("hwtarget", &mic_->hwTarget, "agc",
        "HW target level (raw, ±0.01 dead zone)", 0.05f, 0.9f);
    settings_.registerBool("fastagc", &mic_->fastAgcEnabled, "agc",
        "Enable fast AGC for low-level sources");
    settings_.registerFloat("fastagcthresh", &mic_->fastAgcThreshold, "agc",
        "Raw level threshold for fast AGC", 0.05f, 0.3f);
    settings_.registerUint16("fastagcperiod", &mic_->fastAgcPeriodMs, "agc",
        "Fast AGC calibration period (ms)", 2000, 15000);
    settings_.registerFloat("fastagctau", &mic_->fastAgcTrackingTau, "agc",
        "Fast AGC tracking time (s)", 1.0f, 15.0f);
    settings_.registerUint8("hwgainmax", &mic_->hwGainMaxSignal, "agc",
        "Max HW gain for AGC (10-80)", 10, 80);
}

// (registerTransientSettings/registerDetectionSettings/registerEnsembleSettings removed v67 — BandFlux pipeline removed)

// === RHYTHM TRACKING SETTINGS (AudioController) ===
void SerialConsole::registerRhythmSettings() {
    // audioCtrl_ guaranteed valid — allocated in setup() before registerSettings()

    // Onset strength signal (OSS) generation
    settings_.registerBool("combbankenabled", &audioCtrl_->combBankEnabled, "rhythm",
        "Enable comb filter bank for tempo validation");
    settings_.registerFloat("combbankfeedback", &audioCtrl_->combBankFeedback, "rhythm",
        "Comb bank resonance (0.85-0.98)", 0.85f, 0.98f);
    // (combxvalconf/combxvalcorr removed — comb bank feeds Bayesian fusion directly)

    // CBSS beat tracking parameters
    settings_.registerFloat("cbssalpha", &audioCtrl_->cbssAlpha, "rhythm",
        "CBSS weighting (0.8-0.95). NN auto-lowers to 0.8 if higher", 0.5f, 0.99f);
    settings_.registerFloat("cbsstight", &audioCtrl_->cbssTightness, "rhythm",
        "CBSS log-Gaussian tightness (higher=stricter tempo)", 1.0f, 20.0f);
    settings_.registerFloat("beatconfdecay", &audioCtrl_->beatConfidenceDecay, "rhythm",
        "Beat confidence decay per frame", 0.9f, 0.999f);
    // (temposnap removed — Bayesian fusion handles tempo transitions)
    settings_.registerFloat("beatoffset", &audioCtrl_->beatTimingOffset, "rhythm",
        "Beat prediction advance in frames (ODF+CBSS delay compensation)", 0.0f, 15.0f);
    settings_.registerFloat("phasecorr", &audioCtrl_->phaseCorrectionStrength, "rhythm",
        "Phase correction toward transients (0=off, 1=full snap)", 0.0f, 1.0f);
    settings_.registerFloat("cbssthresh", &audioCtrl_->cbssThresholdFactor, "rhythm",
        "CBSS adaptive threshold factor (0=off, beat fires only if CBSS > factor*mean)", 0.0f, 2.0f);
    settings_.registerFloat("cbsscontrast", &audioCtrl_->cbssContrast, "rhythm",
        "Power-law ODF contrast before CBSS (1=off, 2=BTrack square, default 2.0)", 0.5f, 4.0f);
    settings_.registerUint8("warmupbeats", &audioCtrl_->cbssWarmupBeats, "rhythm",
        "CBSS warmup beats: lower alpha for first N beats (0=disabled)", 0, 32);
    settings_.registerUint8("onsetsnap", &audioCtrl_->onsetSnapWindow, "rhythm",
        "Snap beat to strongest OSS in last N frames (0=disabled, 8=default)", 0, 16);
    settings_.registerFloat("temposmooth", &audioCtrl_->tempoSmoothingFactor, "rhythm",
        "Tempo EMA smoothing (0.5=fast, 0.99=slow)", 0.5f, 0.99f);
    settings_.registerUint8("odfsmooth", &audioCtrl_->odfSmoothWidth, "rhythm",
        "ODF smooth window (3-11, odd)", 3, 11);
    // (ioi/ft registrations removed v52 — dead code since v28)
    settings_.registerBool("odfmeansub", &audioCtrl_->odfMeanSubEnabled, "rhythm",
        "ODF mean subtraction before ACF. NN auto-enables (smooth baseline)");
    settings_.registerBool("beatboundary", &audioCtrl_->beatBoundaryTempo, "rhythm",
        "Defer tempo changes to beat boundaries (BTrack-style, Phase 2.1)");
    // (unifiedodf removed v67 — BandFlux pipeline removed)
    // (nnbeat removed v68 — FrameBeatNN always active, no toggle)
    settings_.registerBool("nnprofile", &audioCtrl_->nnProfile, "rhythm",
        "Enable [NNPROF] per-operator timing output (default off, clutters serial)");
    settings_.registerBool("adaptodf", &audioCtrl_->adaptiveOdfThresh, "rhythm",
        "Local-mean ODF threshold before autocorrelation (BTrack-style, v32)");
    settings_.registerUint8("odfthreshwin", &audioCtrl_->odfThreshWindow, "rhythm",
        "Adaptive ODF threshold half-window size (samples each side, 5-30)", 5, 30);
    // (onsettrainodf/odfdiff removed v67 — BandFlux pipeline removed)
    // (odfsource registration removed v64 — experimental alternatives deleted)
    settings_.registerBool("densityoctave", &audioCtrl_->densityOctaveEnabled, "rhythm",
        "Onset-density octave penalty in Bayesian posterior (v32)");
    settings_.registerFloat("densityminpb", &audioCtrl_->densityMinPerBeat, "rhythm",
        "Min plausible transients per beat for density octave (0.1-3)", 0.1f, 3.0f);
    settings_.registerFloat("densitymaxpb", &audioCtrl_->densityMaxPerBeat, "rhythm",
        "Max plausible transients per beat for density octave (1-20)", 1.0f, 20.0f);
    settings_.registerFloat("densityexp", &audioCtrl_->densityPenaltyExp, "rhythm",
        "Density penalty Gaussian exponent (higher=sharper, 1-20)", 1.0f, 20.0f);
    settings_.registerFloat("densitytarget", &audioCtrl_->densityTarget, "rhythm",
        "Target transients/beat for density penalty (0=disabled, 1-4 typical)", 0.0f, 10.0f);
    settings_.registerBool("downwardcorrect", &audioCtrl_->downwardCorrectEnabled, "rhythm",
        "Downward harmonic correction 3:2/2:1 (experimental, overcorrects mid-tempo)");
    settings_.registerBool("octavecheck", &audioCtrl_->octaveCheckEnabled, "rhythm",
        "Shadow CBSS octave checker (v32)");
    settings_.registerBool("btrkpipeline", &audioCtrl_->btrkPipeline, "rhythm",
        "BTrack-style tempo pipeline: Viterbi + comb-on-ACF (v33)");
    settings_.registerUint8("btrkthreshwin", &audioCtrl_->btrkThreshWindow, "rhythm",
        "Pipeline adaptive threshold half-window (0=off, 1-5 bins each side)", 0, 5);
    // (fwdfilter/fwdtranssigma/fwdfiltcontrast/fwdfiltlambda/fwdfiltfloor/fwdbayesbias/fwdasymmetry removed v64)
    // (hmm/fwdphase/hmmcontrast/fwdobslambda/fwdobsfloor/fwdwrapfrac removed v64)
    // (particlefilter and all pf* registrations removed v64)
    settings_.registerUint8("octavecheckbeats", &audioCtrl_->octaveCheckBeats, "rhythm",
        "Check octave every N beats (2-16)", 2, 16);
    settings_.registerFloat("octavescoreratio", &audioCtrl_->octaveScoreRatio, "rhythm",
        "CBSS score ratio needed for octave switch (1-5)", 1.0f, 5.0f);
    // (phasecheck/plpphase/plpstrength/plpminconf registrations removed v44 — features deleted)
    settings_.registerFloat("rayleighbpm", &audioCtrl_->rayleighBpm, "rhythm",
        "Rayleigh prior peak BPM (v44)", 60.0f, 180.0f);
    settings_.registerFloat("temponudge", &audioCtrl_->tempoNudge, "rhythm",
        "switchTempo posterior mass transfer fraction (v44: 0=none, 1=full swap)", 0.0f, 1.0f);
    settings_.registerBool("fold32", &audioCtrl_->fold32Enabled, "rhythm",
        "3:2 octave folding: fold comb evidence from 2L/3 into L (v44)");
    settings_.registerBool("sesquicheck", &audioCtrl_->sesquiCheckEnabled, "rhythm",
        "3:2 shadow octave check: test 3T/2 and 2T/3 alternatives (v44)");
    // bisnap requires onsetSnapWindow > 0 to have any effect (snap logic is skipped when window=0)
    settings_.registerBool("bisnap", &audioCtrl_->bidirectionalSnap, "rhythm",
        "Bidirectional onset snap: delay beat 3 frames for forward snap window (v44)");
    // (harmonicsesqui registration removed v44 — feature deleted)

    // Percival ACF harmonic pre-enhancement (v45)
    settings_.registerBool("percival", &audioCtrl_->percivalEnhance, "rhythm",
        "Percival harmonic pre-enhancement: fold 2nd/4th ACF harmonics into fundamental (v45)");
    settings_.registerFloat("percivalw2", &audioCtrl_->percivalWeight2, "rhythm",
        "Percival 2nd harmonic fold weight (v45)", 0.0f, 1.0f);
    settings_.registerFloat("percivalw4", &audioCtrl_->percivalWeight4, "rhythm",
        "Percival 4th harmonic fold weight (v45)", 0.0f, 1.0f);

    // PLL phase correction (v45)
    settings_.registerBool("pll", &audioCtrl_->pllEnabled, "rhythm",
        "PLL proportional+integral phase correction at beat fires (v45)");
    settings_.registerFloat("pllkp", &audioCtrl_->pllKp, "rhythm",
        "PLL proportional gain (v45)", 0.0f, 1.0f);
    settings_.registerFloat("pllki", &audioCtrl_->pllKi, "rhythm",
        "PLL integral gain (v45)", 0.0f, 0.1f);
    settings_.registerUint8("pllwarmup", &audioCtrl_->pllWarmupBeats, "rhythm",
        "PLL warmup beats before tightening clamp (v65)", 0, 20);

    // Onset snap hysteresis (v65)
    settings_.registerFloat("snaphyst", &audioCtrl_->snapHysteresis, "rhythm",
        "Snap hysteresis: prefer prev offset if >ratio of best (v65)", 0.0f, 1.0f);

    // Downbeat calibration (v65)
    settings_.registerFloat("dbema", &audioCtrl_->dbEmaAlpha, "rhythm",
        "Downbeat EMA smoothing alpha (v65)", 0.05f, 0.9f);
    settings_.registerFloat("dbthresh", &audioCtrl_->dbThreshold, "rhythm",
        "Smoothed downbeat activation threshold (v65)", 0.1f, 0.9f);
    settings_.registerFloat("dbdecay", &audioCtrl_->dbDecay, "rhythm",
        "Per-frame downbeat decay between beats (v65)", 0.5f, 0.99f);

    // Adaptive CBSS tightness (v45)
    settings_.registerBool("adaptight", &audioCtrl_->adaptiveTightnessEnabled, "rhythm",
        "Adaptive tightness: modulate cbssTightness by onset confidence (v45)");
    settings_.registerFloat("tightlowmult", &audioCtrl_->tightnessLowMult, "rhythm",
        "Tightness multiplier when onset confidence HIGH (looser) (v45)", 0.3f, 1.0f);
    settings_.registerFloat("tighthighmult", &audioCtrl_->tightnessHighMult, "rhythm",
        "Tightness multiplier when onset confidence LOW (tighter) (v45)", 1.0f, 3.0f);
    settings_.registerFloat("tightconfhi", &audioCtrl_->tightnessConfThreshHigh, "rhythm",
        "OSS/mean ratio above this = high onset confidence (v45)", 1.5f, 10.0f);
    settings_.registerFloat("tightconflo", &audioCtrl_->tightnessConfThreshLow, "rhythm",
        "OSS/mean ratio below this = low onset confidence (v45)", 0.5f, 3.0f);

    // (multiagent/agentdecay/agentinitbeats removed v64)

    // Anti-harmonic 3rd comb (v48)
    settings_.registerFloat("percivalw3", &audioCtrl_->percivalWeight3, "rhythm",
        "3rd harmonic SUBTRACT weight: suppress 3:2 ratio confusion (v48)", 0.0f, 1.0f);

    // (metricalcheck/metricalminratio/metricalcheckbeats removed v64)
    // (templatecheck/templatescoreratio/templatecheckbeats removed v64)
    // (subbeatcheck/alternationthresh/subbeatcheckbeats removed v64)
    // (templateminscore/subbeatbins/templatehistbars removed v64)
    settings_.registerFloat("cbssmeanalpha", &audioCtrl_->cbssMeanAlpha, "rhythm",
        "CBSS running mean EMA alpha (v51)", 0.001f, 0.1f);
    settings_.registerFloat("harm2xthresh", &audioCtrl_->harmonic2xThresh, "rhythm",
        "ACF half-lag ratio for 2x BPM correction (v51)", 0.1f, 0.9f);
    settings_.registerFloat("harm15xthresh", &audioCtrl_->harmonic15xThresh, "rhythm",
        "ACF 2/3-lag ratio for 1.5x BPM correction (v51)", 0.1f, 0.9f);
    settings_.registerFloat("pllsmoother", &audioCtrl_->pllSmoother, "rhythm",
        "PLL phase integral leaky decay (v51)", 0.8f, 0.99f);
    settings_.registerFloat("beatconfboost", &audioCtrl_->beatConfBoost, "rhythm",
        "Confidence increment per beat fire (v51)", 0.01f, 0.5f);
    settings_.registerFloat("rhythmblend", &audioCtrl_->rhythmBlend, "rhythm",
        "Periodicity weight in rhythmStrength (v51)", 0.0f, 1.0f);
    settings_.registerFloat("periodicityblend", &audioCtrl_->periodicityBlend, "rhythm",
        "Periodicity strength EMA coefficient (v51)", 0.3f, 0.95f);
    settings_.registerFloat("onsetdensityblend", &audioCtrl_->onsetDensityBlend, "rhythm",
        "Onset density EMA coefficient (v51)", 0.3f, 0.95f);

    // (BandFlux detector settings removed v67 — BandFlux pipeline removed)

    // Bayesian tempo fusion weights (v18+)
    settings_.registerFloat("bayeslambda", &audioCtrl_->bayesLambda, "bayesian",
        "Transition tightness (0.01=rigid, 1.0=loose)", 0.01f, 1.0f);
    settings_.registerFloat("bayesprior", &audioCtrl_->bayesPriorCenter, "bayesian",
        "Static prior center BPM", 60.0f, 200.0f);
    settings_.registerFloat("bayespriorw", &audioCtrl_->bayesPriorWeight, "bayesian",
        "Ongoing static prior strength (0=off, 1=std, 2=strong)", 0.0f, 3.0f);
    settings_.registerFloat("bayesacf", &audioCtrl_->bayesAcfWeight, "bayesian",
        "Autocorrelation observation weight", 0.0f, 5.0f);
    // (bayesft/bayesioi registrations removed v52 — dead code since v28)
    settings_.registerFloat("bayescomb", &audioCtrl_->bayesCombWeight, "bayesian",
        "Comb filter bank observation weight", 0.0f, 5.0f);
    settings_.registerFloat("postfloor", &audioCtrl_->posteriorFloor, "bayesian",
        "Posterior uniform floor to prevent mode lock (0=off)", 0.0f, 0.5f);
    settings_.registerFloat("disambignudge", &audioCtrl_->disambigNudge, "bayesian",
        "Posterior nudge when disambiguation corrects (0=off)", 0.0f, 0.5f);
    settings_.registerFloat("harmonictrans", &audioCtrl_->harmonicTransWeight, "bayesian",
        "Transition matrix harmonic shortcut weight (0=off)", 0.0f, 1.0f);

    // (Ensemble fusion settings removed v67 — BandFlux pipeline removed)

    // Basic rhythm activation and output modulation
    settings_.registerFloat("musicthresh", &audioCtrl_->activationThreshold, "rhythm",
        "Rhythm activation threshold (0-1)", 0.0f, 1.0f);
    settings_.registerFloat("pulseboost", &audioCtrl_->pulseBoostOnBeat, "rhythm",
        "Pulse boost on beat", 1.0f, 2.0f);
    settings_.registerFloat("pulsesuppress", &audioCtrl_->pulseSuppressOffBeat, "rhythm",
        "Pulse suppress off beat", 0.3f, 1.0f);
    settings_.registerFloat("energyboost", &audioCtrl_->energyBoostOnBeat, "rhythm",
        "Energy boost on beat", 0.0f, 1.0f);
    settings_.registerFloat("bpmmin", &audioCtrl_->bpmMin, "rhythm",
        "Minimum BPM to detect", 40.0f, 120.0f);
    settings_.registerFloat("bpmmax", &audioCtrl_->bpmMax, "rhythm",
        "Maximum BPM to detect", 80.0f, 240.0f);

    // Autocorrelation timing
    settings_.registerUint16("autocorrperiod", &audioCtrl_->autocorrPeriodMs, "rhythm",
        "Autocorr period (ms)", 100, 1000);

    // (bassbandweight/midbandweight/highbandweight removed v67 — BandFlux pipeline removed)

    // Tempo prior width (used by Bayesian static prior initialization)
    settings_.registerFloat("priorwidth", &audioCtrl_->tempoPriorWidth, "bayesian",
        "Prior width (sigma BPM)", 10.0f, 80.0f);

    // Beat stability tracking
    settings_.registerFloat("stabilitywin", &audioCtrl_->stabilityWindowBeats, "stability",
        "Stability window (beats)", 4.0f, 16.0f);

    // Beat lookahead (anticipatory effects)
    settings_.registerFloat("lookahead", &audioCtrl_->beatLookaheadMs, "lookahead",
        "Beat lookahead (ms)", 0.0f, 200.0f);

    // Continuous tempo estimation
    settings_.registerFloat("tempochgthresh", &audioCtrl_->tempoChangeThreshold, "tempo",
        "Tempo change threshold", 0.01f, 0.5f);
    // (maxbpmchg removed — Bayesian fusion handles tempo stability)

    // Spectral processing (whitening + compressor)
    SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
    settings_.registerBool("whitenenabled", &spectral.whitenEnabled, "spectral",
        "Per-bin spectral whitening");
    settings_.registerFloat("whitendecay", &spectral.whitenDecay, "spectral",
        "Whitening peak decay per frame (0.99-0.999)", 0.9f, 0.9999f);
    settings_.registerFloat("whitenfloor", &spectral.whitenFloor, "spectral",
        "Whitening noise floor", 0.0001f, 0.1f);
    settings_.registerBool("whitenbassbypass", &spectral.whitenBassBypass, "spectral",
        "Skip whitening for bass bins 1-6 (preserve kick contrast, v47)");
    settings_.registerBool("compenabled", &spectral.compressorEnabled, "spectral",
        "Soft-knee compressor");
    settings_.registerFloat("compthresh", &spectral.compThresholdDb, "spectral",
        "Compressor threshold (dB)", -60.0f, 0.0f);
    settings_.registerFloat("compratio", &spectral.compRatio, "spectral",
        "Compression ratio", 1.0f, 20.0f);
    settings_.registerFloat("compknee", &spectral.compKneeDb, "spectral",
        "Soft knee width (dB)", 0.0f, 30.0f);
    settings_.registerFloat("compmakeup", &spectral.compMakeupDb, "spectral",
        "Makeup gain (dB)", -10.0f, 30.0f);
    settings_.registerFloat("compattack", &spectral.compAttackTau, "spectral",
        "Attack time constant (s)", 0.0001f, 0.1f);
    settings_.registerFloat("comprelease", &spectral.compReleaseTau, "spectral",
        "Release time constant (s)", 0.01f, 10.0f);

    // Noise estimation (v56: minimum statistics + spectral subtraction)
    settings_.registerBool("noiseest", &spectral.noiseEstEnabled, "spectral",
        "Spectral noise subtraction (Martin 2001)");
    settings_.registerFloat("noisesmooth", &spectral.noiseSmoothAlpha, "spectral",
        "Noise power smoothing (0.8-0.99)", 0.8f, 0.999f);
    settings_.registerFloat("noiserelease", &spectral.noiseReleaseFactor, "spectral",
        "Noise floor release rate (0.99-0.9999)", 0.99f, 0.9999f);
    settings_.registerFloat("noiseover", &spectral.noiseOversubtract, "spectral",
        "Oversubtraction factor (1.0-3.0)", 0.5f, 5.0f);
    settings_.registerFloat("noisefloor", &spectral.noiseFloorRatio, "spectral",
        "Spectral floor ratio (0.001-0.5)", 0.001f, 0.5f);
}

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
        checkBayesianInteractions();
        return;
    }

    // Then try special commands (JSON API, config management)
    if (handleSpecialCommand(cmd)) {
        return;
    }

    Serial.println(F("Unknown command. Try 'settings' for help."));
}

bool SerialConsole::handleSpecialCommand(const char* cmd) {
    // Assert error counter
    if (strcmp(cmd, "show errors") == 0) {
        Serial.print(F("{\"assertFails\":"));
        Serial.print(BlinkyAssert::failCount);
        Serial.println(F("}"));
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
    if (handleTestCommand(cmd)) return true;
    if (handleAudioStatusCommand(cmd)) return true;
    if (handleModeCommand(cmd)) return true;
    if (handleConfigCommand(cmd)) return true;
    if (handleDeviceConfigCommand(cmd)) return true;  // Device config commands (v28+)
    if (handleLogCommand(cmd)) return true;
    if (handleDebugCommand(cmd)) return true;     // Debug channel commands
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
        Serial.print(F("{\"version\":\""));
        Serial.print(F(BLINKY_VERSION_STRING));
        Serial.print(F("\""));

        // Device configuration status (v28+)
        if (configStorage_ && configStorage_->isDeviceConfigValid()) {
            const ConfigStorage::StoredDeviceConfig& cfg = configStorage_->getDeviceConfig();
            Serial.print(F(",\"device\":{\"id\":\""));
            Serial.print(cfg.deviceId);
            Serial.print(F("\",\"name\":\""));
            Serial.print(cfg.deviceName);
            Serial.print(F("\",\"width\":"));
            Serial.print(cfg.ledWidth);
            Serial.print(F(",\"height\":"));
            Serial.print(cfg.ledHeight);
            Serial.print(F(",\"leds\":"));
            Serial.print(cfg.ledWidth * cfg.ledHeight);
            Serial.print(F(",\"configured\":true}"));
        } else {
            Serial.print(F(",\"device\":{\"configured\":false,\"safeMode\":true}"));
        }

        Serial.println(F("}"));
        return true;
    }

    if (strcmp(cmd, "json state") == 0) {
        if (!pipeline_) {
            Serial.println(F("{\"error\":\"Pipeline not available\"}"));
            return true;
        }
        Serial.print(F("{\"generator\":\""));
        Serial.print(pipeline_->getGeneratorName());
        Serial.print(F("\",\"effect\":\""));
        Serial.print(pipeline_->getEffectName());
        Serial.print(F("\",\"generators\":["));
        for (int i = 0; i < RenderPipeline::NUM_GENERATORS; i++) {
            if (i > 0) Serial.print(',');
            Serial.print('"');
            Serial.print(RenderPipeline::getGeneratorNameByIndex(i));
            Serial.print('"');
        }
        Serial.print(F("],\"effects\":["));
        for (int i = 0; i < RenderPipeline::NUM_EFFECTS; i++) {
            if (i > 0) Serial.print(',');
            Serial.print('"');
            Serial.print(RenderPipeline::getEffectNameByIndex(i));
            Serial.print('"');
        }
        Serial.println(F("]}"));
        return true;
    }

    return false;
}

// === BATTERY COMMANDS ===
bool SerialConsole::handleBatteryCommand(const char* cmd) {
    if (strcmp(cmd, "battery debug") == 0 || strcmp(cmd, "batt debug") == 0) {
        if (battery_) {
            Serial.println(F("=== Battery Debug Info ==="));
            Serial.print(F("Connected: "));
            Serial.println(battery_->isBatteryConnected() ? F("Yes") : F("No"));
            Serial.print(F("Voltage: "));
            Serial.print(battery_->getVoltage(), 3);
            Serial.println(F("V"));
            Serial.print(F("Percent: "));
            Serial.print(battery_->getPercent());
            Serial.println(F("%"));
            Serial.print(F("Charging: "));
            Serial.println(battery_->isCharging() ? F("Yes") : F("No"));
            Serial.println(F("(Use 'battery raw' for detailed ADC values)"));
        } else {
            Serial.println(F("Battery monitor not available"));
        }
        return true;
    }

    if (strcmp(cmd, "battery") == 0 || strcmp(cmd, "batt") == 0) {
        if (battery_) {
            float voltage = battery_->getVoltage();
            uint8_t percent = battery_->getPercent();
            bool charging = battery_->isCharging();
            bool connected = battery_->isBatteryConnected();

            Serial.print(F("{\"battery\":{"));
            Serial.print(F("\"voltage\":"));
            Serial.print(voltage, 2);
            Serial.print(F(",\"percent\":"));
            Serial.print(percent);
            Serial.print(F(",\"charging\":"));
            Serial.print(charging ? F("true") : F("false"));
            Serial.print(F(",\"connected\":"));
            Serial.print(connected ? F("true") : F("false"));
            Serial.println(F("}}"));
        } else {
            Serial.println(F("{\"error\":\"Battery monitor not available\"}"));
        }
        return true;
    }

    return false;
}

// === STREAM COMMANDS ===
bool SerialConsole::handleStreamCommand(const char* cmd) {
    if (strcmp(cmd, "stream on") == 0) {
        streamEnabled_ = true;
        Serial.println(F("OK"));
        return true;
    }

    if (strcmp(cmd, "stream off") == 0) {
        streamEnabled_ = false;
        streamNN_ = false;
        Serial.println(F("OK"));
        return true;
    }

    if (strcmp(cmd, "stream debug") == 0) {
        streamEnabled_ = true;
        streamDebug_ = true;
        Serial.println(F("OK debug"));
        return true;
    }

    if (strcmp(cmd, "stream normal") == 0) {
        streamDebug_ = false;
        streamFast_ = false;
        streamNN_ = false;
        Serial.println(F("OK normal"));
        return true;
    }

    if (strcmp(cmd, "stream nn") == 0) {
        streamEnabled_ = false;  // Disable timer-based stream to avoid TX overflow
        streamNN_ = true;        // NN stream fires independently on isFrameReady()
        streamFast_ = false;
        Serial.println(F("OK nn"));
        return true;
    }

    if (strcmp(cmd, "stream fast") == 0) {
        streamEnabled_ = true;
        streamFast_ = true;
        Serial.println(F("OK fast"));
        return true;
    }

    return false;
}

// === TEST MODE COMMANDS ===
bool SerialConsole::handleTestCommand(const char* cmd) {
    if (strncmp(cmd, "test lock hwgain", 16) == 0) {
        // Ensure command is exact match or followed by space (not "test lock hwgainXYZ")
        if (cmd[16] != '\0' && cmd[16] != ' ') {
            return false;
        }
        if (!mic_) {
            Serial.println(F("ERROR: Microphone not available"));
            return true;
        }
        // Parse optional gain value (default to current gain)
        int gain = mic_->getHwGain();
        if (strlen(cmd) > 17) {
            gain = atoi(cmd + 17);
            if (gain < 0 || gain > 80) {
                Serial.print(F("WARNING: Gain "));
                Serial.print(gain);
                Serial.println(F(" out of range (0-80), will be clamped"));
            }
        }
        mic_->lockHwGain(gain);
        Serial.print(F("OK locked at "));
        Serial.println(mic_->getHwGain());
        return true;
    }

    if (strcmp(cmd, "test unlock hwgain") == 0) {
        if (!mic_) {
            Serial.println(F("ERROR: Microphone not available"));
            return true;
        }
        mic_->unlockHwGain();
        Serial.println(F("OK unlocked"));
        return true;
    }

    return false;
}

// === AUDIO CONTROLLER STATUS ===
bool SerialConsole::handleAudioStatusCommand(const char* cmd) {
    if (strcmp(cmd, "music") == 0 || strcmp(cmd, "rhythm") == 0 || strcmp(cmd, "audio") == 0) {
        if (audioCtrl_) {
            const AudioControl& audio = audioCtrl_->getControl();
            Serial.println(F("=== Audio Controller Status ==="));
            Serial.print(F("Rhythm Active: "));
            Serial.println(audio.rhythmStrength > audioCtrl_->activationThreshold ? F("YES") : F("NO"));
            Serial.print(F("BPM: "));
            Serial.println(audioCtrl_->getCurrentBpm(), 1);
            Serial.print(F("Phase: "));
            Serial.println(audio.phase, 2);
            Serial.print(F("Rhythm Strength: "));
            Serial.println(audio.rhythmStrength, 2);
            Serial.print(F("Periodicity: "));
            Serial.println(audioCtrl_->getPeriodicityStrength(), 2);
            Serial.print(F("Energy: "));
            Serial.println(audio.energy, 2);
            Serial.print(F("Pulse: "));
            Serial.println(audio.pulse, 2);
            Serial.print(F("Onset Density: "));
            Serial.print(audio.onsetDensity, 1);
            Serial.println(F(" /s"));
            Serial.print(F("BPM Range: "));
            Serial.print(audioCtrl_->getBpmMin(), 0);
            Serial.print(F("-"));
            Serial.println(audioCtrl_->getBpmMax(), 0);

            // New metrics from research-based improvements
            Serial.println(F("--- Advanced Metrics ---"));
            Serial.print(F("Beat Stability: "));
            Serial.println(audioCtrl_->getBeatStability(), 2);
            Serial.print(F("Tempo Velocity: "));
            Serial.print(audioCtrl_->getTempoVelocity(), 1);
            Serial.println(F(" BPM/s"));
            Serial.print(F("Next Beat In: "));
            uint32_t nowMs = millis();
            uint32_t nextMs = audioCtrl_->getNextBeatMs();
            Serial.print(nextMs > nowMs ? (nextMs - nowMs) : 0);
            Serial.println(F(" ms"));
            Serial.print(F("Bayesian Prior Center: "));
            Serial.print(audioCtrl_->bayesPriorCenter, 0);
            Serial.print(F(" BPM (best bin conf="));
            Serial.print(audioCtrl_->getBayesBestConf(), 2);
            Serial.println(F(")"));
        } else {
            Serial.println(F("Audio controller not available"));
        }
        return true;
    }

    return false;
}

// === DETECTION MODE STATUS ===
bool SerialConsole::handleModeCommand(const char* cmd) {
    if (strcmp(cmd, "mode") == 0) {
        Serial.println(F("=== Audio Detection Status ==="));
        if (audioCtrl_) {
            Serial.print(F("Pulse Strength: "));
            Serial.println(audioCtrl_->getLastPulseStrength(), 3);
            Serial.print(F("BPM: "));
            Serial.println(audioCtrl_->getCurrentBpm(), 1);
            Serial.print(F("CBSS Confidence: "));
            Serial.println(audioCtrl_->getCbssConfidence(), 3);
            Serial.print(F("Beat Count: "));
            Serial.println(audioCtrl_->getBeatCount());
        } else {
            Serial.println(F("AudioController not available"));
        }
        if (mic_) {
            Serial.print(F("Audio Level: "));
            Serial.println(mic_->getLevel(), 3);
            Serial.print(F("Hardware Gain: "));
            Serial.println(mic_->getHwGain());
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
            Serial.println(F("OK"));
        } else {
            Serial.println(F("ERROR"));
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
            checkBayesianInteractions();
            Serial.println(F("OK"));
        } else {
            Serial.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "defaults") == 0) {
        restoreDefaults();
        checkBayesianInteractions();
        Serial.println(F("OK"));
        return true;
    }

    if (strcmp(cmd, "reset") == 0 || strcmp(cmd, "factory") == 0) {
        if (configStorage_) {
            configStorage_->factoryReset();
            restoreDefaults();
            Serial.println(F("OK"));
        } else {
            Serial.println(F("ERROR"));
        }
        return true;
    }

    if (strcmp(cmd, "reboot") == 0) {
        Serial.println(F("Rebooting..."));
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
        Serial.println(F("Entering UF2 bootloader..."));
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
        Serial.println(F("UF2 bootloader not available on this platform"));
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

    Serial.println(F("Device configuration commands:"));
    Serial.println(F("  device show          - Display current device config"));
    Serial.println(F("  device upload <JSON> - Upload device config from JSON"));
    Serial.println(F("\nExample JSON at: devices/registry/README.md"));
    return false;
}

void SerialConsole::showDeviceConfig() {
    if (!configStorage_) {
        Serial.println(F("{\"error\":\"ConfigStorage not available\"}"));
        return;
    }

    if (!configStorage_->isDeviceConfigValid()) {
        Serial.println(F("{\"error\":\"No device config\",\"status\":\"unconfigured\",\"safeMode\":true}"));
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
    Serial.println();
}

void SerialConsole::uploadDeviceConfig(const char* jsonStr) {
    if (!configStorage_) {
        Serial.println(F("ERROR: ConfigStorage not available"));
        return;
    }

    // Parse JSON using ArduinoJson (1024 bytes to accommodate full device configs ~600 bytes)
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, jsonStr);

    if (error) {
        Serial.print(F("ERROR: JSON parse failed - "));
        Serial.println(error.c_str());
        Serial.println(F("Example: device upload {\"deviceId\":\"hat_v1\",\"ledWidth\":89,...}"));
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
        Serial.println(F("ERROR: Device config validation failed"));
        Serial.println(F("Check LED count, pin numbers, and voltage ranges"));
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
            *mic_,
            audioCtrl_
        );
    } else {
        Serial.println(F("ERROR: Cannot save config - mic not initialized"));
        return;
    }

    Serial.println(F("✓ Device config saved to flash"));
    Serial.print(F("Device: "));
    Serial.print(newConfig.deviceName);
    Serial.print(F(" ("));
    Serial.print(newConfig.ledWidth * newConfig.ledHeight);
    Serial.println(F(" LEDs)"));
    Serial.println(F("\n**REBOOT DEVICE TO APPLY CONFIGURATION**"));
}

void SerialConsole::restoreDefaults() {
    // NOTE: Particle-based generators get defaults from their constructors
    // Generator parameter reset is handled by ConfigStorage::loadDefaults()
    // which will be applied on next load/save cycle

    // Restore mic defaults (window/range normalization)
    // Note: Transient detection now handled by ODF-derived pulse detection (v67)
    if (mic_) {
        mic_->peakTau = Defaults::PeakTau;              // 2s peak adaptation
        mic_->releaseTau = Defaults::ReleaseTau;        // 5s peak release
        mic_->hwTarget = 0.20f;                         // Target raw input level (lower = less gain seeking)

        // Fast AGC defaults (enabled by default for better low-level response)
        mic_->fastAgcEnabled = true;
        mic_->fastAgcThreshold = 0.15f;
        mic_->fastAgcPeriodMs = 5000;
        mic_->fastAgcTrackingTau = 5.0f;
    }

    // Restore audio controller defaults
    if (audioCtrl_) {
        audioCtrl_->activationThreshold = 0.4f;
        audioCtrl_->cbssAlpha = 0.9f;
        audioCtrl_->cbssTightness = 8.0f;           // v40: raised from 5.0 (+24% avg F1)
        audioCtrl_->beatConfidenceDecay = 0.98f;
        audioCtrl_->bayesLambda = 0.60f;
        audioCtrl_->bayesPriorCenter = 128.0f;
        audioCtrl_->bayesPriorWeight = 0.0f;
        audioCtrl_->bayesAcfWeight = 0.8f;
        audioCtrl_->bayesCombWeight = 0.7f;
        audioCtrl_->posteriorFloor = 0.05f;
        audioCtrl_->disambigNudge = 0.15f;
        audioCtrl_->harmonicTransWeight = 0.30f;
        audioCtrl_->cbssThresholdFactor = 1.0f;
        audioCtrl_->beatBoundaryTempo = true;
        // (unifiedOdf default removed v67 — BandFlux pipeline removed)
        audioCtrl_->odfMeanSubEnabled = false;   // v32: raw ODF better than mean-subtracted
        audioCtrl_->adaptiveOdfThresh = false;
        audioCtrl_->densityOctaveEnabled = true;  // v32: onset-density octave penalty
        audioCtrl_->densityMinPerBeat = 0.5f;
        audioCtrl_->densityMaxPerBeat = 5.0f;
        audioCtrl_->downwardCorrectEnabled = false; // v41: experimental, overcorrects mid-tempo
        audioCtrl_->octaveCheckEnabled = true;    // v32: shadow CBSS octave checker
        audioCtrl_->octaveCheckBeats = 2;         // v32: aggressive (every 2 beats)
        audioCtrl_->octaveScoreRatio = 1.3f;      // v32: aggressive threshold
        // (forwardFilter/fwd*/fwdPhaseOnly defaults removed v64 — forward filter deleted)
        audioCtrl_->btrkPipeline = true;          // v33: BTrack pipeline (Viterbi + comb-on-ACF)
        audioCtrl_->btrkThreshWindow = 0;         // v33: adaptive threshold OFF (too aggressive with 20 bins)
        // (barPointerHmm/hmmContrast/fwdObsLambda/fwdObsFloor/fwdWrapFraction defaults removed v64)
        audioCtrl_->cbssContrast = 2.0f;           // v66: ODF contrast before CBSS (A/B tested 10-6 win)
        audioCtrl_->cbssWarmupBeats = 0;           // v37: CBSS warmup disabled
        audioCtrl_->onsetSnapWindow = 8;           // v39: snap beat to strongest OSS in ±8 frames
        audioCtrl_->odfThreshWindow = 15;          // v35: adaptive ODF threshold half-window
        // (onsetTrainOdf/odfDiffMode defaults removed v67 — BandFlux pipeline removed)
        // (odfSource default removed v64 — experimental alternatives deleted)
        audioCtrl_->densityPenaltyExp = 2.0f;      // v32: density penalty exponent
        audioCtrl_->densityTarget = 0.0f;          // v32: density target (0=disabled)
        // (phaseCheck/PLP defaults removed v44 — features deleted)

        // Percival ACF harmonic pre-enhancement (v45)
        audioCtrl_->percivalEnhance = true;
        audioCtrl_->percivalWeight2 = 0.5f;
        audioCtrl_->percivalWeight4 = 0.25f;

        // PLL phase correction (v45)
        audioCtrl_->pllEnabled = true;
        audioCtrl_->pllKp = 0.15f;
        audioCtrl_->pllKi = 0.005f;

        // Adaptive CBSS tightness (v45)
        audioCtrl_->adaptiveTightnessEnabled = true;
        audioCtrl_->tightnessLowMult = 0.7f;
        audioCtrl_->tightnessHighMult = 1.3f;
        audioCtrl_->tightnessConfThreshHigh = 3.0f;
        audioCtrl_->tightnessConfThreshLow = 1.5f;

        audioCtrl_->percivalWeight3 = 0.0f;
        // (multiAgent/metrical/template/subbeat defaults removed v64 — features deleted)

        // Hidden calibration constants (v51)
        audioCtrl_->cbssMeanAlpha = 0.008f;
        audioCtrl_->harmonic2xThresh = 0.5f;
        audioCtrl_->harmonic15xThresh = 0.6f;
        audioCtrl_->pllSmoother = 0.95f;
        audioCtrl_->beatConfBoost = 0.15f;
        audioCtrl_->rhythmBlend = 0.6f;
        audioCtrl_->periodicityBlend = 0.7f;
        audioCtrl_->onsetDensityBlend = 0.7f;
        // (subbeatBins/templateHistBars defaults removed v64)
        // (particleFilter defaults removed v64 — PF deleted)
        audioCtrl_->tempoSmoothingFactor = 0.85f;
        audioCtrl_->pulseBoostOnBeat = 1.3f;
        audioCtrl_->pulseSuppressOffBeat = 0.6f;
        audioCtrl_->energyBoostOnBeat = 0.3f;
        audioCtrl_->bpmMin = 60.0f;
        audioCtrl_->bpmMax = 200.0f;

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
        Serial.println(F("Available generators:"));
        for (int i = 0; i < RenderPipeline::NUM_GENERATORS; i++) {
            const char* name = RenderPipeline::getGeneratorNameByIndex(i);
            bool active = (RenderPipeline::getGeneratorTypeByIndex(i) == pipeline_->getGeneratorType());
            Serial.print(F("  "));
            Serial.print(name);
            if (active) Serial.print(F(" (active)"));
            Serial.println();
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
        }

        if (found) {
            if (pipeline_->setGenerator(type)) {
                Serial.print(F("OK switched to "));
                Serial.println(pipeline_->getGeneratorName());
            } else {
                Serial.println(F("ERROR: Failed to switch generator"));
            }
        } else {
            Serial.print(F("Unknown generator: "));
            Serial.println(name);
            Serial.println(F("Use: fire, water, lightning, audio"));
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
        Serial.println(F("Available effects:"));
        for (int i = 0; i < RenderPipeline::NUM_EFFECTS; i++) {
            const char* name = RenderPipeline::getEffectNameByIndex(i);
            bool active = (RenderPipeline::getEffectTypeByIndex(i) == pipeline_->getEffectType());
            Serial.print(F("  "));
            Serial.print(name);
            if (active) Serial.print(F(" (active)"));
            Serial.println();
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
                Serial.print(F("OK effect: "));
                Serial.println(pipeline_->getEffectName());
            } else {
                Serial.println(F("ERROR: Failed to set effect"));
            }
        } else {
            Serial.print(F("Unknown effect: "));
            Serial.println(name);
            Serial.println(F("Use: none, hue"));
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

void SerialConsole::checkBayesianInteractions() {
    // audioCtrl_ guaranteed valid — allocated in setup() before any commands
    // (FT/IOI interaction warning removed v52 — FT/IOI dead code removed)
}

void SerialConsole::streamTick() {
    if (!streamEnabled_ && !streamNN_) return;

    uint32_t now = millis();

    // NN diagnostic stream: fires every spectral frame (~62.5 Hz)
    // Outputs the exact mel bands fed to the NN + NN output for offline validation.
    // Format: {"type":"NN","ts":<ms>,"mel":[26 floats],"beat":<float>,"db":<float>,"bpm":<float>}
    // "mel" = getRawMelBands() — the exact input to FrameBeatNN::infer()
    // "beat" = NN beat activation output (0 if NN not loaded)
    // "db" = NN downbeat activation (0 if no downbeat head)
    // "bpm" = current estimated tempo
    if (streamNN_ && audioCtrl_) {
        const SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
        uint32_t fc = spectral.getFrameCount();
        if (fc != lastNNFrameCount_) {
            lastNNFrameCount_ = fc;
            const float* mel = spectral.getRawMelBands();

            Serial.print(F("{\"type\":\"NN\",\"ts\":"));
            Serial.print(now);
            Serial.print(F(",\"mel\":["));
            for (int i = 0; i < SpectralConstants::NUM_MEL_BANDS; i++) {
                if (i > 0) Serial.print(',');
                Serial.print(mel[i], 4);
            }
            // onset = raw ODF fed into CBSS (NN activation or mic level fallback)
            Serial.print(F("],\"onset\":"));
            Serial.print(audioCtrl_->getLastOnsetStrength(), 4);
            // Note (v65): "nn" field is now always present in both NN and non-NN builds.
            // Previously it was ifdef-guarded; non-NN builds emitted "nn":0 via #else.
            // The stub's isReady() returns false, so the value is still 0 in non-NN builds.
            Serial.print(F(",\"nn\":"));
            Serial.print(audioCtrl_->getFrameBeatNN().isReady() ? 1 : 0);
            if (audioCtrl_->getFrameBeatNN().isReady()) {
                Serial.print(F(",\"nndb\":"));
                Serial.print(audioCtrl_->getFrameBeatNN().getLastDownbeat(), 4);
            }
            Serial.print(F(",\"bpm\":"));
            Serial.print(audioCtrl_->getCurrentBpm(), 1);
            Serial.print(F(",\"phase\":"));
            Serial.print(audioCtrl_->getControl().phase, 4);
            Serial.print(F(",\"rstr\":"));
            Serial.print(audioCtrl_->getControl().rhythmStrength, 3);
            Serial.print(F(",\"lvl\":"));
            Serial.print(mic_->getLevel(), 3);
            Serial.print(F(",\"gain\":"));
            Serial.print(mic_->getHwGain());
            Serial.println(F("}"));
        }
    }

    // STATUS update at ~1Hz
    static uint32_t lastStatusMs = 0;
    if (mic_ && (now - lastStatusMs >= 1000)) {
        lastStatusMs = now;
        Serial.print(F("{\"type\":\"STATUS\",\"ts\":"));
        Serial.print(now);
        Serial.print(F(",\"mode\":\"ensemble\""));
        Serial.print(F(",\"hwGain\":"));
        Serial.print(mic_->getHwGain());
        Serial.print(F(",\"level\":"));
        Serial.print(mic_->getLevel(), 2);
        Serial.print(F(",\"peakLevel\":"));
        Serial.print(mic_->getPeakLevel(), 2);
        Serial.println(F("}"));
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
        Serial.print(F("{\"a\":{\"l\":"));
        Serial.print(mic_->getLevel(), 2);
        Serial.print(F(",\"t\":"));
        // Pulse strength from ODF-derived pulse detection (v67)
        float transient = 0.0f;
        if (audioCtrl_) {
            transient = audioCtrl_->getLastPulseStrength();
        }
        Serial.print(transient, 2);
        Serial.print(F(",\"pk\":"));
        Serial.print(mic_->getPeakLevel(), 2);
        Serial.print(F(",\"vl\":"));
        Serial.print(mic_->getValleyLevel(), 2);
        Serial.print(F(",\"raw\":"));
        Serial.print(mic_->getRawLevel(), 2);
        Serial.print(F(",\"h\":"));
        Serial.print(mic_->getHwGain());
        Serial.print(F(",\"alive\":"));
        Serial.print(mic_->isPdmAlive() ? 1 : 0);

        // Debug mode: add pulse and spectral state
        // (BandFlux per-band flux fields removed v67 — BandFlux pipeline removed)
        if (streamDebug_ && audioCtrl_) {
            Serial.print(F(",\"pulse\":"));
            Serial.print(audioCtrl_->getLastPulseStrength(), 3);

            // Spectral processing state (compressor + whitening)
            const SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
            Serial.print(F(",\"rms\":"));
            Serial.print(spectral.getFrameRmsDb(), 1);
            Serial.print(F(",\"cg\":"));
            Serial.print(spectral.getSmoothedGainDb(), 2);
        }

        Serial.print(F("}"));

        // AudioController telemetry (unified rhythm tracking)
        // Format: "m":{"a":1,"bpm":125.3,"ph":0.45,"str":0.82,"conf":0.75,"bc":42,"q":0,"bt":12345,"e":0.5,"p":0.8,"cb":0.12,"oss":0.05,"ttb":18,"bp":1,"od":3.2,"db":0.8,"bm":1}
        // a = rhythm active, bpm = tempo, ph = phase, str = rhythm strength
        // conf = CBSS confidence, bc = beat count, q = beat event (phase wrap)
        // bt = firmware millis() at beat (only when q=1 and timestamp>0)
        // e = energy, p = pulse, cb = CBSS value, oss = onset strength
        // ttb = frames until next beat, bp = last beat was predicted (1) vs fallback (0)
        // od = onset density (onsets/second, EMA smoothed)
        // db = downbeat activation (beat-synchronized), bm = beat in measure (1-4, 0=unknown)
        if (audioCtrl_) {
            const AudioControl& audio = audioCtrl_->getControl();

            // Detect beat events via phase wrapping (>0.8 → <0.2)
            static float lastStreamPhase = 0.0f;
            float currentPhase = audio.phase;
            int beatEvent = (lastStreamPhase > 0.8f && currentPhase < 0.2f && audio.rhythmStrength > audioCtrl_->activationThreshold) ? 1 : 0;
            lastStreamPhase = currentPhase;

            Serial.print(F(",\"m\":{\"a\":"));
            Serial.print(audio.rhythmStrength > audioCtrl_->activationThreshold ? 1 : 0);
            Serial.print(F(",\"bpm\":"));
            Serial.print(audioCtrl_->getCurrentBpm(), 1);
            Serial.print(F(",\"ph\":"));
            Serial.print(currentPhase, 2);
            Serial.print(F(",\"str\":"));
            Serial.print(audio.rhythmStrength, 2);
            Serial.print(F(",\"conf\":"));
            Serial.print(audioCtrl_->getCbssConfidence(), 2);
            Serial.print(F(",\"bc\":"));
            Serial.print(audioCtrl_->getBeatCount());
            Serial.print(F(",\"q\":"));
            Serial.print(beatEvent);
            // Precise firmware beat timestamp (millis) for MCP latency reduction
            if (beatEvent && audioCtrl_->getLastBeatTimeMs() > 0) {
                Serial.print(F(",\"bt\":"));
                Serial.print(audioCtrl_->getLastBeatTimeMs());
            }
            Serial.print(F(",\"e\":"));
            Serial.print(audio.energy, 2);
            Serial.print(F(",\"p\":"));
            Serial.print(audio.pulse, 2);
            Serial.print(F(",\"cb\":"));
            Serial.print(audioCtrl_->getCurrentCBSS(), 3);
            Serial.print(F(",\"oss\":"));
            Serial.print(audioCtrl_->getLastOnsetStrength(), 3);
            Serial.print(F(",\"ttb\":"));
            Serial.print(audioCtrl_->getTimeToNextBeat());
            Serial.print(F(",\"bp\":"));
            Serial.print(audioCtrl_->wasLastBeatPredicted() ? 1 : 0);
            Serial.print(F(",\"od\":"));
            Serial.print(audioCtrl_->getOnsetDensity(), 1);
            Serial.print(F(",\"db\":"));
            Serial.print(audio.downbeat, 2);
            Serial.print(F(",\"bm\":"));
            Serial.print(audio.beatInMeasure);

            // Debug mode: add Bayesian tempo state for tuning
            if (streamDebug_) {
                Serial.print(F(",\"ps\":"));
                Serial.print(audioCtrl_->getPeriodicityStrength(), 3);
                Serial.print(F(",\"bb\":"));
                Serial.print(audioCtrl_->getBayesBestBin());
                Serial.print(F(",\"bbc\":"));
                Serial.print(audioCtrl_->getBayesBestConf(), 4);
                Serial.print(F(",\"bcb\":"));
                Serial.print(audioCtrl_->getBayesCombObs(), 3);
                // (PLP streaming fields removed v44 — feature deleted)
            }

            Serial.print(F("}"));
        }

        // LED brightness telemetry
        // NOTE: Particle-based generators don't track total heat/brightness
        // in the same way, so these stats are not available
        // TODO: Add particle pool statistics (active count, etc.)

        Serial.println(F("}"));
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
        Serial.print(F("{\"b\":{\"n\":"));
        Serial.print(battery_->isBatteryConnected() ? F("true") : F("false"));
        Serial.print(F(",\"c\":"));
        Serial.print(battery_->isCharging() ? F("true") : F("false"));
        Serial.print(F(",\"v\":"));
        Serial.print(battery_->getVoltage(), 2);
        Serial.print(F(",\"p\":"));
        Serial.print(battery_->getPercent());
        Serial.println(F("}}"));
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
        Serial.print(F("Log level: "));
        switch (logLevel_) {
            case LogLevel::OFF:   Serial.println(F("off")); break;
            case LogLevel::ERROR: Serial.println(F("error")); break;
            case LogLevel::WARN:  Serial.println(F("warn")); break;
            case LogLevel::INFO:  Serial.println(F("info")); break;
            case LogLevel::DEBUG: Serial.println(F("debug")); break;
        }
        return true;
    }

    // "log off" - disable logging
    if (strcmp(cmd, "log off") == 0) {
        logLevel_ = LogLevel::OFF;
        Serial.println(F("OK log off"));
        return true;
    }

    // "log error" - errors only
    if (strcmp(cmd, "log error") == 0) {
        logLevel_ = LogLevel::ERROR;
        Serial.println(F("OK log error"));
        return true;
    }

    // "log warn" - warnings and errors
    if (strcmp(cmd, "log warn") == 0) {
        logLevel_ = LogLevel::WARN;
        Serial.println(F("OK log warn"));
        return true;
    }

    // "log info" - info and above (default)
    if (strcmp(cmd, "log info") == 0) {
        logLevel_ = LogLevel::INFO;
        Serial.println(F("OK log info"));
        return true;
    }

    // "log debug" - all messages
    if (strcmp(cmd, "log debug") == 0) {
        logLevel_ = LogLevel::DEBUG;
        Serial.println(F("OK log debug"));
        return true;
    }

    return false;
}

// === DEBUG CHANNEL COMMANDS ===
// Controls per-subsystem JSON debug output independently from log levels
bool SerialConsole::handleDebugCommand(const char* cmd) {
    // "debug" - show enabled channels
    if (strcmp(cmd, "debug") == 0) {
        Serial.println(F("Debug channels:"));
        Serial.print(F("  transient:  ")); Serial.println(isDebugChannelEnabled(DebugChannel::TRANSIENT) ? F("ON") : F("off"));
        Serial.print(F("  rhythm:     ")); Serial.println(isDebugChannelEnabled(DebugChannel::RHYTHM) ? F("ON") : F("off"));
        Serial.print(F("  audio:      ")); Serial.println(isDebugChannelEnabled(DebugChannel::AUDIO) ? F("ON") : F("off"));
        Serial.print(F("  generator:  ")); Serial.println(isDebugChannelEnabled(DebugChannel::GENERATOR) ? F("ON") : F("off"));
        Serial.print(F("  ensemble:   ")); Serial.println(isDebugChannelEnabled(DebugChannel::ENSEMBLE) ? F("ON") : F("off"));
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
                Serial.print(F("Unknown channel: "));
                Serial.println(channelName);
                Serial.println(F("Valid: transient, rhythm, audio, generator, ensemble, all"));
                return true;
            }

            const char* action = space + 1;
            if (strcmp(action, "on") == 0) {
                enableDebugChannel(channel);
                Serial.print(F("OK debug "));
                Serial.print(channelName);
                Serial.println(F(" on"));
                return true;
            } else if (strcmp(action, "off") == 0) {
                disableDebugChannel(channel);
                Serial.print(F("OK debug "));
                Serial.print(channelName);
                Serial.println(F(" off"));
                return true;
            } else {
                Serial.print(F("Invalid action: "));
                Serial.println(action);
                Serial.println(F("Use 'on' or 'off'"));
                return true;
            }
        }

        Serial.println(F("Usage: debug <channel> on|off"));
        Serial.println(F("Channels: transient, rhythm, audio, generator, ensemble, all"));
        return true;
    }

    return false;
}

// === BEAT TRACKING COMMANDS ===
bool SerialConsole::handleBeatTrackingCommand(const char* cmd) {
    if (!audioCtrl_) {
        Serial.println(F("Audio controller not available"));
        return false;
    }

    // "show nn" - NN beat activation diagnostics (moved from handleEnsembleCommand v67)
    if (strcmp(cmd, "show nn") == 0) {
        audioCtrl_->getFrameBeatNN().printDiagnostics();
        if (audioCtrl_->getFrameBeatNN().isReady()) {
            float contrast = audioCtrl_->cbssContrast;
            float alpha = audioCtrl_->cbssAlpha;
            bool meanSub = audioCtrl_->odfMeanSubEnabled;
            Serial.print(F("[NN] params: contrast="));
            Serial.print(contrast);
            Serial.print(F(" alpha="));
            Serial.print((alpha > 0.8f) ? 0.8f : alpha);
            Serial.print(F(" odfMeanSub=on"));
            if (!meanSub) Serial.print(F(" (auto)"));
            Serial.println();
        }
        return true;
    }

    // === PULSE MODULATION THRESHOLDS (moved from handleEnsembleCommand v67) ===
    if (strncmp(cmd, "set pulsenear ", 14) == 0) {
        float value = atof(cmd + 14);
        if (value >= 0.0f && value <= 0.5f) {
            audioCtrl_->pulseNearBeatThreshold = value;
            Serial.print(F("OK pulsenear="));
            Serial.println(value, 3);
        } else {
            Serial.println(F("ERROR: Valid range 0.0-0.5"));
        }
        return true;
    }
    if (strcmp(cmd, "show pulsenear") == 0 || strcmp(cmd, "pulsenear") == 0) {
        Serial.print(F("pulsenear="));
        Serial.println(audioCtrl_->pulseNearBeatThreshold, 3);
        return true;
    }
    if (strncmp(cmd, "set pulsefar ", 13) == 0) {
        float value = atof(cmd + 13);
        if (value >= 0.2f && value <= 0.5f) {
            audioCtrl_->pulseFarFromBeatThreshold = value;
            Serial.print(F("OK pulsefar="));
            Serial.println(value, 3);
        } else {
            Serial.println(F("ERROR: Valid range 0.2-0.5"));
        }
        return true;
    }
    if (strcmp(cmd, "show pulsefar") == 0 || strcmp(cmd, "pulsefar") == 0) {
        Serial.print(F("pulsefar="));
        Serial.println(audioCtrl_->pulseFarFromBeatThreshold, 3);
        return true;
    }

    // "show beat" - show CBSS beat tracking state
    if (strcmp(cmd, "show beat") == 0) {
        Serial.println(F("=== CBSS Beat Tracker ==="));
        Serial.print(F("BPM: "));
        Serial.println(audioCtrl_->getCurrentBpm(), 1);
        Serial.print(F("Phase: "));
        Serial.println(audioCtrl_->getControl().phase, 3);
        Serial.print(F("Confidence: "));
        Serial.println(audioCtrl_->getCbssConfidence(), 3);
        Serial.print(F("Beat Count: "));
        Serial.println(audioCtrl_->getBeatCount());
        Serial.print(F("Beat Period (samples): "));
        Serial.println(audioCtrl_->getBeatPeriodSamples());
        Serial.print(F("Periodicity: "));
        Serial.println(audioCtrl_->getPeriodicityStrength(), 3);
        Serial.print(F("Stability: "));
        Serial.println(audioCtrl_->getBeatStability(), 3);
        Serial.print(F("Onset Density: "));
        Serial.print(audioCtrl_->getOnsetDensity(), 1);
        Serial.println(F(" /s"));
        Serial.print(F("Downbeat: "));
        Serial.println(audioCtrl_->getControl().downbeat, 2);
        Serial.print(F("Beat in Measure: "));
        Serial.println(audioCtrl_->getControl().beatInMeasure);
        Serial.println();
        return true;
    }

    // "json rhythm" - output rhythm tracking state as JSON (for test automation)
    if (strcmp(cmd, "json rhythm") == 0) {
        Serial.print(F("{\"bpm\":"));
        Serial.print(audioCtrl_->getCurrentBpm(), 1);
        Serial.print(F(",\"periodicityStrength\":"));
        Serial.print(audioCtrl_->getPeriodicityStrength(), 3);
        Serial.print(F(",\"beatStability\":"));
        Serial.print(audioCtrl_->getBeatStability(), 3);
        Serial.print(F(",\"tempoVelocity\":"));
        Serial.print(audioCtrl_->getTempoVelocity(), 2);
        Serial.print(F(",\"nextBeatMs\":"));
        Serial.print(audioCtrl_->getNextBeatMs());
        Serial.print(F(",\"bayesBestConf\":"));
        Serial.print(audioCtrl_->getBayesBestConf(), 3);
        Serial.print(F(",\"phase\":"));
        Serial.print(audioCtrl_->getControl().phase, 3);
        Serial.print(F(",\"rhythmStrength\":"));
        Serial.print(audioCtrl_->getControl().rhythmStrength, 3);
        Serial.print(F(",\"cbssConfidence\":"));
        Serial.print(audioCtrl_->getCbssConfidence(), 3);
        Serial.print(F(",\"beatCount\":"));
        Serial.print(audioCtrl_->getBeatCount());
        Serial.print(F(",\"onsetDensity\":"));
        Serial.print(audioCtrl_->getOnsetDensity(), 1);
        Serial.print(F(",\"downbeat\":"));
        Serial.print(audioCtrl_->getControl().downbeat, 2);
        Serial.print(F(",\"beatInMeasure\":"));
        Serial.print(audioCtrl_->getControl().beatInMeasure);
        Serial.println(F("}"));
        return true;
    }

    // "json beat" - output CBSS beat tracker state as JSON
    if (strcmp(cmd, "json beat") == 0) {
        Serial.print(F("{\"bpm\":"));
        Serial.print(audioCtrl_->getCurrentBpm(), 1);
        Serial.print(F(",\"phase\":"));
        Serial.print(audioCtrl_->getControl().phase, 3);
        Serial.print(F(",\"periodicity\":"));
        Serial.print(audioCtrl_->getPeriodicityStrength(), 3);
        Serial.print(F(",\"confidence\":"));
        Serial.print(audioCtrl_->getCbssConfidence(), 3);
        Serial.print(F(",\"beatCount\":"));
        Serial.print(audioCtrl_->getBeatCount());
        Serial.print(F(",\"beatPeriod\":"));
        Serial.print(audioCtrl_->getBeatPeriodSamples());
        Serial.print(F(",\"stability\":"));
        Serial.print(audioCtrl_->getBeatStability(), 3);
        Serial.print(F(",\"downbeat\":"));
        Serial.print(audioCtrl_->getControl().downbeat, 2);
        Serial.print(F(",\"beatInMeasure\":"));
        Serial.print(audioCtrl_->getControl().beatInMeasure);
        Serial.println(F("}"));
        return true;
    }

    // "show spectral" - show spectral processing (compressor + whitening) state
    if (strcmp(cmd, "show spectral") == 0) {
        const SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
        Serial.println(F("=== Spectral Processing ==="));
        Serial.println(F("-- Compressor --"));
        Serial.print(F("  Enabled: ")); Serial.println(spectral.compressorEnabled ? "yes" : "no");
        Serial.print(F("  Threshold: ")); Serial.print(spectral.compThresholdDb, 1); Serial.println(F(" dB"));
        Serial.print(F("  Ratio: ")); Serial.print(spectral.compRatio, 1); Serial.println(F(":1"));
        Serial.print(F("  Knee: ")); Serial.print(spectral.compKneeDb, 1); Serial.println(F(" dB"));
        Serial.print(F("  Makeup: ")); Serial.print(spectral.compMakeupDb, 1); Serial.println(F(" dB"));
        Serial.print(F("  Attack: ")); Serial.print(spectral.compAttackTau * 1000.0f, 1); Serial.println(F(" ms"));
        Serial.print(F("  Release: ")); Serial.print(spectral.compReleaseTau, 2); Serial.println(F(" s"));
        Serial.print(F("  Frame RMS: ")); Serial.print(spectral.getFrameRmsDb(), 1); Serial.println(F(" dB"));
        Serial.print(F("  Smoothed Gain: ")); Serial.print(spectral.getSmoothedGainDb(), 2); Serial.println(F(" dB"));
        Serial.println(F("-- Whitening --"));
        Serial.print(F("  Enabled: ")); Serial.println(spectral.whitenEnabled ? "yes" : "no");
        Serial.print(F("  Decay: ")); Serial.println(spectral.whitenDecay, 4);
        Serial.print(F("  Floor: ")); Serial.println(spectral.whitenFloor, 4);
        Serial.println();
        return true;
    }

    // "json spectral" - spectral processing state as JSON (for test automation)
    if (strcmp(cmd, "json spectral") == 0) {
        const SharedSpectralAnalysis& spectral = audioCtrl_->getSpectral();
        Serial.print(F("{\"compEnabled\":"));
        Serial.print(spectral.compressorEnabled ? 1 : 0);
        Serial.print(F(",\"compThreshDb\":"));
        Serial.print(spectral.compThresholdDb, 1);
        Serial.print(F(",\"compRatio\":"));
        Serial.print(spectral.compRatio, 1);
        Serial.print(F(",\"compKneeDb\":"));
        Serial.print(spectral.compKneeDb, 1);
        Serial.print(F(",\"compMakeupDb\":"));
        Serial.print(spectral.compMakeupDb, 1);
        Serial.print(F(",\"compAttackMs\":"));
        Serial.print(spectral.compAttackTau * 1000.0f, 2);
        Serial.print(F(",\"compReleaseS\":"));
        Serial.print(spectral.compReleaseTau, 2);
        Serial.print(F(",\"rmsDb\":"));
        Serial.print(spectral.getFrameRmsDb(), 1);
        Serial.print(F(",\"gainDb\":"));
        Serial.print(spectral.getSmoothedGainDb(), 2);
        Serial.print(F(",\"whitenEnabled\":"));
        Serial.print(spectral.whitenEnabled ? 1 : 0);
        Serial.print(F(",\"whitenDecay\":"));
        Serial.print(spectral.whitenDecay, 4);
        Serial.print(F(",\"whitenFloor\":"));
        Serial.print(spectral.whitenFloor, 4);
        Serial.println(F("}"));
        return true;
    }

    return false;
}

// === LOGGING HELPERS ===
void SerialConsole::logDebug(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::DEBUG) {
        Serial.print(F("[DEBUG] "));
        Serial.println(msg);
    }
}

void SerialConsole::logInfo(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::INFO) {
        Serial.print(F("[INFO] "));
        Serial.println(msg);
    }
}

void SerialConsole::logWarn(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::WARN) {
        Serial.print(F("[WARN] "));
        Serial.println(msg);
    }
}

void SerialConsole::logError(const __FlashStringHelper* msg) {
    if (getGlobalLogLevel() >= LogLevel::ERROR) {
        Serial.print(F("[ERROR] "));
        Serial.println(msg);
    }
}
