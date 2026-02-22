/**
 * Enhanced metadata for settings and metrics
 * Provides user-friendly display names, detailed tooltips, and units
 */

export interface SettingMetadata {
  displayName: string;
  tooltip: string;
  unit?: string;
}

/**
 * Settings metadata - Enhanced labels and tooltips for device settings
 */
export const settingsMetadata: Record<string, SettingMetadata> = {
  // ============================================================
  // FIRE GENERATOR (Particle-based)
  // ============================================================
  basespawnchance: {
    displayName: 'Base Spawn Chance',
    tooltip: 'Baseline probability of spawning sparks per frame (0-1). Higher = more active fire.',
    unit: '',
  },
  audiospawnboost: {
    displayName: 'Audio Spawn Boost',
    tooltip: 'Audio reactivity multiplier (0-2). Amplifies spawn rate based on energy level.',
    unit: '',
  },
  maxparticles: {
    displayName: 'Max Particles',
    tooltip:
      'Maximum number of particles in the pool (1-64). More particles = denser effects but higher CPU. Pool capacity is 64.',
    unit: '',
  },
  defaultlifespan: {
    displayName: 'Default Lifespan',
    tooltip:
      'Default particle lifespan in centiseconds (1-255, where 100 = 1 second). Higher = longer-lived particles that rise further.',
    unit: 'cs',
  },
  intensitymin: {
    displayName: 'Min Intensity',
    tooltip: 'Minimum spawn intensity/brightness (0-255). Lower allows dimmer particles.',
    unit: '',
  },
  intensitymax: {
    displayName: 'Max Intensity',
    tooltip: 'Maximum spawn intensity/brightness (0-255). Higher creates brighter particles.',
    unit: '',
  },
  gravity: {
    displayName: 'Gravity',
    tooltip:
      'Gravity strength applied per frame. Negative = upward (fire rises), positive = downward (water falls).',
    unit: '',
  },
  windbase: {
    displayName: 'Base Wind',
    tooltip:
      'Sustained directional drift force (LEDs/sec²). Positive = rightward lean, negative = leftward. Applied as acceleration so effect builds over particle lifetime.',
    unit: 'LEDs/sec²',
  },
  windvariation: {
    displayName: 'Wind Turbulence',
    tooltip:
      'Curl-noise turbulence intensity (LEDs/sec). Directly displaces particles laterally each frame — windVariation=10 moves a particle ~0.17 LEDs/frame sideways. Set to 5-15 for subtle sway, 20-40 for strong swirling.',
    unit: 'LEDs/sec',
  },
  drag: {
    displayName: 'Drag',
    tooltip:
      'Drag coefficient for per-frame velocity damping (0-1). Closer to 1 = less drag, particles move freely.',
    unit: '',
  },
  sparkvelmin: {
    displayName: 'Min Spark Velocity',
    tooltip:
      'Minimum upward velocity for sparks (LEDs/sec). Lower = slower rise. At 60fps a value of 10 moves ~0.17 LEDs per frame.',
    unit: 'LEDs/sec',
  },
  sparkvelmax: {
    displayName: 'Max Spark Velocity',
    tooltip:
      'Maximum upward velocity for sparks (LEDs/sec). Higher = faster rise and greater height reached.',
    unit: 'LEDs/sec',
  },
  sparkspread: {
    displayName: 'Spark Spread',
    tooltip: 'Horizontal velocity variation for sparks (LEDs/sec). Higher = wider lateral spread.',
    unit: 'LEDs/sec',
  },
  burstsparks: {
    displayName: 'Burst Spark Count',
    tooltip: 'Number of sparks generated per beat burst (1-20). More = bigger reactions.',
    unit: 'sparks',
  },

  fastsparks: {
    displayName: 'Fast Spark Ratio',
    tooltip:
      'Fraction of spawned particles that are fast sparks (0-1). Remainder are slow embers with heavier trails and longer lifespan. 0 = all embers, 1 = all sparks.',
    unit: '',
  },
  thermalforce: {
    displayName: 'Thermal Force',
    tooltip:
      'Thermal buoyancy strength in LEDs/sec². Particles over hot regions are pushed upward by this force scaled by local heat (0 = no buoyancy, 30 = default, 100 = strong).',
    unit: 'LEDs/sec²',
  },

  bgintensity: {
    displayName: 'Background Intensity',
    tooltip:
      'Noise background brightness (0-1). Sets the underlayer glow beneath particles. 0 = pitch black, 0.15 = subtle, 0.5 = bright ambient.',
    unit: '',
  },

  // Fire: Music Mode
  musicspawnpulse: {
    displayName: 'Beat Spawn Depth',
    tooltip:
      'How deeply spawn rate breathes with the beat (0-1). 0 = no modulation (constant spawning), 0.5 = gentle pulsing, 0.95 = near-silent off-beat with strong on-beat bursts. Controls pool drain between beats to make burst sparks visible.',
    unit: '',
  },

  // Fire: Organic Mode
  organictransmin: {
    displayName: 'Organic Transient Min',
    tooltip:
      'Minimum transient level to trigger burst in organic mode (0-1). Higher = requires stronger hits.',
    unit: '',
  },

  // ============================================================
  // WATER GENERATOR (Particle-based)
  // ============================================================
  dropvelmin: {
    displayName: 'Min Drop Velocity',
    tooltip: 'Minimum downward velocity for drops (LEDs/sec). Lower = slower falling.',
    unit: 'LEDs/sec',
  },
  dropvelmax: {
    displayName: 'Max Drop Velocity',
    tooltip: 'Maximum downward velocity for drops (LEDs/sec). Higher = faster falling.',
    unit: 'LEDs/sec',
  },
  dropspread: {
    displayName: 'Drop Spread',
    tooltip: 'Horizontal velocity variation for drops (LEDs/sec). Higher = wider spray.',
    unit: 'LEDs/sec',
  },
  splashparticles: {
    displayName: 'Splash Particle Count',
    tooltip: 'Number of particles spawned on splash impact (0-10). More = bigger splashes.',
    unit: 'particles',
  },
  splashvelmin: {
    displayName: 'Min Splash Velocity',
    tooltip: 'Minimum splash velocity (LEDs/sec). Lower = smaller splashes.',
    unit: 'LEDs/sec',
  },
  splashvelmax: {
    displayName: 'Max Splash Velocity',
    tooltip: 'Maximum splash velocity (LEDs/sec). Higher = bigger splashes.',
    unit: 'LEDs/sec',
  },
  splashintensity: {
    displayName: 'Splash Intensity',
    tooltip: 'Splash particle intensity multiplier (0-255). Higher = brighter splashes.',
    unit: '',
  },

  // ============================================================
  // LIGHTNING GENERATOR (Particle-based)
  // ============================================================
  boltvelmin: {
    displayName: 'Min Bolt Speed',
    tooltip: 'Minimum bolt speed (LEDs/sec). Lower = slower bolts.',
    unit: 'LEDs/sec',
  },
  boltvelmax: {
    displayName: 'Max Bolt Speed',
    tooltip: 'Maximum bolt speed (LEDs/sec). Higher = faster bolts.',
    unit: 'LEDs/sec',
  },
  faderate: {
    displayName: 'Fade Rate',
    tooltip: 'Intensity decay per frame (0-255). Higher = faster fade, shorter bolts.',
    unit: '',
  },
  branchcount: {
    displayName: 'Branch Count',
    tooltip: 'Number of branches spawned per trigger (1-4). More = bushier lightning.',
    unit: 'branches',
  },
  branchspread: {
    displayName: 'Branch Angle Spread',
    tooltip: 'Angle variation for branches (radians). Higher = wider forking.',
    unit: 'rad',
  },
  branchintloss: {
    displayName: 'Branch Intensity Loss',
    tooltip: 'Intensity reduction for child branches (0-100%). Higher = dimmer branches.',
    unit: '%',
  },

  // ============================================================
  // AUDIO VISUALIZATION GENERATOR (Diagnostic display)
  // ============================================================
  transientrowfrac: {
    displayName: 'Transient Row Fraction',
    tooltip:
      'Fraction of display height for transient indicator (0.1-0.5). Green gradient from top showing pulse/transient intensity.',
    unit: '',
  },
  transientdecay: {
    displayName: 'Transient Decay Rate',
    tooltip:
      'How fast transient indicator fades per frame (0.01-0.5). Higher = faster fade after transient.',
    unit: '',
  },
  transientbright: {
    displayName: 'Transient Brightness',
    tooltip: 'Maximum brightness of transient indicator (0-255). Green gradient intensity.',
    unit: '',
  },
  levelbright: {
    displayName: 'Level Brightness',
    tooltip: 'Brightness of audio level row (0-255). Yellow row whose Y position indicates energy.',
    unit: '',
  },
  levelsmooth: {
    displayName: 'Level Smoothing',
    tooltip:
      'Smoothing factor for energy level changes (0-0.99). Higher = smoother but slower response.',
    unit: '',
  },
  phasebright: {
    displayName: 'Phase Brightness',
    tooltip:
      'Maximum brightness of phase row (0-255). Blue row showing beat phase, modulated by rhythm confidence.',
    unit: '',
  },
  musicmodethresh: {
    displayName: 'Music Mode Threshold',
    tooltip:
      'Minimum rhythm confidence to show phase indicator (0-1). Phase row hidden when rhythm not detected.',
    unit: '',
  },
  beatpulsebright: {
    displayName: 'Beat Pulse Brightness',
    tooltip:
      'Maximum brightness of center beat pulse band (0-255). Blue flash on each detected beat.',
    unit: '',
  },
  beatpulsedecay: {
    displayName: 'Beat Pulse Decay',
    tooltip: 'How fast beat pulse fades per frame (0.01-0.5). Higher = faster fade after beat.',
    unit: '',
  },
  beatpulsewidth: {
    displayName: 'Beat Pulse Width',
    tooltip:
      'Fraction of display height for beat pulse band (0.05-0.5). Centered vertically with soft edges.',
    unit: '',
  },
  bgbright: {
    displayName: 'Background Brightness',
    tooltip: 'Dim background brightness (0-255). Subtle background for contrast.',
    unit: '',
  },

  // Audio settings (window/range normalization)
  peaktau: {
    displayName: 'Peak Adaptation',
    tooltip:
      'How quickly the peak tracker adapts to louder signals (0.5-10s). Lower = faster peak tracking, higher = smoother.',
    unit: 's',
  },
  releasetau: {
    displayName: 'Peak Release',
    tooltip:
      'How slowly the peak tracker releases when audio gets quieter (1-30s). Higher = sustains peaks longer.',
    unit: 's',
  },

  // Onset detection settings (freq category)
  onsetthresh: {
    displayName: 'Onset Threshold',
    tooltip:
      'Energy must exceed baseline by this factor to trigger onset (1.5-5x). Higher = fewer false positives, may miss subtle hits.',
    unit: 'x',
  },
  risethresh: {
    displayName: 'Rise Threshold',
    tooltip:
      'Energy must rise by this factor from previous frame (1.1-2x). Higher = requires sharper transients.',
    unit: 'x',
  },

  // Hardware AGC settings (agc category)
  hwtarget: {
    displayName: 'HW Target Level',
    tooltip:
      'Target raw ADC level for hardware gain (0.05-0.9). Has ±0.01 dead zone. Hardware gain adapts to keep raw input near this target for optimal signal quality.',
    unit: '',
  },
  hwtargetlow: {
    displayName: 'HW Target Low',
    tooltip:
      'Minimum raw ADC level target. Hardware gain increases if raw level stays below this (0.05-0.5).',
    unit: '',
  },
  hwtargethigh: {
    displayName: 'HW Target High',
    tooltip:
      'Maximum raw ADC level target. Hardware gain decreases if raw level exceeds this (0.1-0.9).',
    unit: '',
  },

  // Software AGC settings (envelope-based gain control)
  agenabled: {
    displayName: 'Auto-Gain Enabled',
    tooltip:
      'Enable automatic gain control. AGC adapts gain to make loud peaks reach full dynamic range (100%), ensuring optimal use of available headroom.',
    unit: '',
  },
  agcattack: {
    displayName: 'Peak Attack',
    tooltip:
      'How quickly the AGC envelope follows sudden increases (0.01-5s). Lower = catches peaks faster. Default: 0.1s (100ms) for responsive peak tracking.',
    unit: 's',
  },
  agcrelease: {
    displayName: 'Peak Release',
    tooltip:
      'How slowly the AGC envelope decays after peaks (0.1-10s). Higher = smoother tracking, preserves dynamics. Default: 2s.',
    unit: 's',
  },
  agcgaintau: {
    displayName: 'Gain Adaptation Speed',
    tooltip:
      'How quickly gain adjusts to match the peak target (0.1-30s). Higher = smoother but slower adaptation. Lower = faster but more reactive. Default: 5s.',
    unit: 's',
  },

  // Transient detection settings (transient category)
  hitthresh: {
    displayName: 'Hit Threshold',
    tooltip:
      'Signal must be this many times louder than recent average to trigger (1.5-10x). Higher = fewer false positives, may miss soft hits. Tuned via param-tuner.',
    unit: 'x',
  },
  attackmult: {
    displayName: 'Attack Multiplier',
    tooltip:
      'Signal must rise by this factor from previous frame (1.1-2x). Detects "sudden" rises. Higher = only sharp attacks trigger. Default: 1.2 (20% rise required).',
    unit: 'x',
  },
  avgtau: {
    displayName: 'Average Tracking Time',
    tooltip:
      'Time constant for recent average level tracking (0.1-5s). Lower = more responsive to level changes, higher = smoother baseline. Default: 0.8s.',
    unit: 's',
  },
  cooldown: {
    displayName: 'Hit Cooldown',
    tooltip:
      'Minimum time between transient detections (20-500ms). Prevents double-triggering. Lower = can detect rapid hits, higher = filters drum rolls. Default: 30ms (~33 hits/sec max).',
    unit: 'ms',
  },

  // Detection mode settings (detection category)
  detectmode: {
    displayName: 'Detection Algorithm',
    tooltip:
      'Transient detection algorithm: 0=Drummer (amplitude), 1=Bass Band (filtered), 2=HFC (high freq), 3=Spectral Flux (FFT), 4=Hybrid (drummer+flux). Hybrid recommended (best F1).',
    unit: '',
  },

  // Bass Band mode parameters (mode 1)
  bassfreq: {
    displayName: 'Bass Cutoff Frequency',
    tooltip:
      'Bass filter cutoff frequency (40-200 Hz). Lower = only deep kicks, higher = includes bass + low toms. Default: 120 Hz for kick drums.',
    unit: 'Hz',
  },
  bassq: {
    displayName: 'Bass Filter Q',
    tooltip:
      'Bass filter Q factor (0.5-3.0). Lower = wider frequency response (Butterworth), higher = sharper filter. Default: 1.0.',
    unit: '',
  },
  bassthresh: {
    displayName: 'Bass Threshold',
    tooltip:
      'Bass energy detection threshold (1.5-10x). Similar to hitthresh but for bass-filtered signal. Higher = only strong kicks trigger. Default: 3.0.',
    unit: 'x',
  },

  // HFC mode parameters (mode 2)
  hfcweight: {
    displayName: 'HFC Weighting',
    tooltip:
      'High-frequency content weighting factor (0.5-5.0). Higher = emphasizes sharp attacks (snares, claps). Lower = more balanced. Default: 1.0.',
    unit: '',
  },
  hfcthresh: {
    displayName: 'HFC Threshold',
    tooltip:
      'HFC detection threshold (1.5-10x). Higher = only very sharp attacks trigger. Good for rejecting sustained sounds. Default: 3.0.',
    unit: 'x',
  },

  // Spectral Flux mode parameters (mode 3)
  fluxthresh: {
    displayName: 'Flux Threshold',
    tooltip:
      'Spectral flux detection threshold (1.0-10.0). Industry-standard FFT-based onset detection. Higher = fewer detections. Tuned to 2.8 via param-tuner.',
    unit: '',
  },
  fluxbins: {
    displayName: 'FFT Bins',
    tooltip:
      'Number of FFT bins to analyze (4-128). More bins = better frequency resolution but higher CPU. Default: 64 bins (bass-mid focus).',
    unit: 'bins',
  },

  // Hybrid mode parameters (mode 4)
  hyfluxwt: {
    displayName: 'Hybrid: Flux Weight',
    tooltip:
      'Weight when only spectral flux detects (0.1-1.0). Higher = trust flux more when drummer disagrees. Tuned to 0.3 via param-tuner.',
    unit: '',
  },
  hydrumwt: {
    displayName: 'Hybrid: Drummer Weight',
    tooltip:
      'Weight when only drummer detects (0.1-1.0). Higher = trust amplitude detection more when flux disagrees. Tuned to 0.3 via param-tuner.',
    unit: '',
  },
  hybothboost: {
    displayName: 'Hybrid: Agreement Boost',
    tooltip:
      'Multiplier when both algorithms agree (1.0-2.0). Boosts confidence when drummer and flux both detect. Higher = stronger reactions. Default: 1.2.',
    unit: 'x',
  },

  // MusicMode settings (music category)
  musicthresh: {
    displayName: 'Activation Threshold',
    tooltip:
      'Confidence required to activate music mode (0.0-1.0). Lower = activates faster but may false-trigger. Higher = more reliable but slower. Default: 0.6.',
    unit: '',
  },
  musicbeats: {
    displayName: 'Beats to Activate',
    tooltip:
      'Number of stable beats required before activation (2-16). More = more reliable lock-on but slower. Fewer = faster but may false-activate. Default: 4.',
    unit: 'beats',
  },
  musicmissed: {
    displayName: 'Max Missed Beats',
    tooltip:
      'Consecutive missed beats before deactivation (4-16). Higher = maintains lock through breakdowns, lower = fails faster when rhythm stops. Default: 8.',
    unit: 'beats',
  },
  confinc: {
    displayName: 'Confidence Gain',
    tooltip:
      'Confidence increase per good beat (0.05-0.2). Higher = faster lock-on but may overreact. Lower = more conservative. Default: 0.1.',
    unit: '',
  },
  confdec: {
    displayName: 'Confidence Loss',
    tooltip:
      'Confidence decrease per bad/missed beat (0.05-0.2). Higher = fails faster when rhythm unclear. Lower = maintains lock longer. Default: 0.1 (symmetric with gain).',
    unit: '',
  },
  phasetol: {
    displayName: 'Phase Tolerance',
    tooltip:
      'Maximum phase error for "good beat" (0.1-0.5). Phase 0-1 within beat cycle. Lower = stricter timing, higher = accepts looser rhythm. Default: 0.2 (20% of beat period).',
    unit: '',
  },
  missedtol: {
    displayName: 'Missed Beat Tolerance',
    tooltip:
      'Beat period multiplier for missed beat detection (1.0-3.0). Time without onset before counting as missed. Higher = more patient. Default: 1.5x beat period.',
    unit: 'x',
  },
  bpmmin: {
    displayName: 'Minimum BPM',
    tooltip:
      'Minimum tempo for music mode (40-120 BPM). Prevents false locks on slow ambient music. Lower = supports ballads, higher = rejects slow drifts. Default: 60 BPM.',
    unit: 'BPM',
  },
  bpmmax: {
    displayName: 'Maximum BPM',
    tooltip:
      'Maximum tempo for music mode (120-240 BPM). Prevents false locks on rapid noise. Higher = supports drum & bass, lower = rejects fast artifacts. Default: 200 BPM.',
    unit: 'BPM',
  },
  pllkp: {
    displayName: 'PLL Proportional Gain',
    tooltip:
      'Phase-locked loop proportional gain (0.01-0.5). Higher = more responsive to phase errors but may overshoot. Lower = smoother. Default: 0.1.',
    unit: '',
  },
  pllki: {
    displayName: 'PLL Integral Gain',
    tooltip:
      'Phase-locked loop integral gain (0.001-0.1). Corrects steady-state phase drift. Higher = faster convergence but may oscillate. Default: 0.01.',
    unit: '',
  },

  // RhythmAnalyzer settings (rhythm category)
  rhythmminbpm: {
    displayName: 'Min BPM (Autocorr)',
    tooltip:
      'Minimum tempo for autocorrelation analysis (60-120 BPM). Lower bound of tempo detection range. Default: 60 BPM.',
    unit: 'BPM',
  },
  rhythmmaxbpm: {
    displayName: 'Max BPM (Autocorr)',
    tooltip:
      'Maximum tempo for autocorrelation analysis (120-240 BPM). Upper bound of tempo detection range. Default: 200 BPM.',
    unit: 'BPM',
  },
  rhythminterval: {
    displayName: 'Autocorr Update Interval',
    tooltip:
      'How often autocorrelation runs (500-2000ms). Lower = more responsive but higher CPU. Higher = smoother but slower. Default: 1000ms (1 second).',
    unit: 'ms',
  },
  beatthresh: {
    displayName: 'Beat Likelihood Threshold',
    tooltip:
      'Threshold for virtual beat synthesis (0.5-0.9). When beat likelihood exceeds this, synthesize beat even without transient. Higher = fewer virtual beats. Default: 0.7.',
    unit: '',
  },
  minperiodicity: {
    displayName: 'Min Periodicity Strength',
    tooltip:
      'Minimum periodicity to consider rhythm detected (0.3-0.8). Autocorrelation confidence threshold. Higher = stricter rhythm requirements. Default: 0.5.',
    unit: '',
  },
};

/**
 * Audio metrics metadata - Enhanced labels for real-time audio monitoring
 */
export const audioMetricsMetadata: Record<string, SettingMetadata> = {
  l: {
    displayName: 'Level',
    tooltip:
      'Post-range-mapping audio level (0-1). Final processed amplitude that drives fire intensity. Automatically normalized using peak/valley window tracking.',
    unit: '',
  },
  t: {
    displayName: 'Transient',
    tooltip:
      'Transient strength (0-1). Maximum of low/high band onset detection. Single-frame impulse when transient detected. Drives fire bursts.',
    unit: '',
  },
  pk: {
    displayName: 'Peak',
    tooltip:
      'Current tracked peak level (raw 0-1 range). The upper bound of the dynamic range window. Adapts to loud signals with fast attack, releases slowly when audio gets quieter.',
    unit: '',
  },
  vl: {
    displayName: 'Valley',
    tooltip:
      'Current tracked valley level (raw 0-1 range). The lower bound of the dynamic range window. Tracks the noise floor and quiet passages.',
    unit: '',
  },
  raw: {
    displayName: 'Raw ADC',
    tooltip:
      'Raw ADC input level (0-1 range, before window/range mapping). This is what the hardware gain system targets to keep in optimal range (0.15-0.35) for best signal quality.',
    unit: '',
  },
  h: {
    displayName: 'HW Gain',
    tooltip:
      'Hardware PDM gain setting (0-80). Primary gain control that adapts slowly to optimize raw ADC signal quality and signal-to-noise ratio.',
    unit: '',
  },
  alive: {
    displayName: 'PDM Status',
    tooltip:
      'PDM microphone status (0=dead, 1=alive). Shows whether the microphone is actively producing audio data.',
    unit: '',
  },
};

/**
 * Battery metrics metadata - Enhanced labels for battery monitoring
 */
export const batteryMetricsMetadata: Record<string, SettingMetadata> = {
  n: {
    displayName: 'Connected',
    tooltip: 'Whether a battery is detected (voltage in valid LiPo range 2.5-4.3V).',
    unit: '',
  },
  c: {
    displayName: 'Charging',
    tooltip: 'Whether the device is currently charging via USB.',
    unit: '',
  },
  v: {
    displayName: 'Voltage',
    tooltip: 'Current battery voltage (2.7-4.2V). 4.2V = full, 3.0V = low.',
    unit: 'V',
  },
  p: {
    displayName: 'Battery %',
    tooltip: 'Estimated battery percentage (0-100%).',
    unit: '%',
  },
};
