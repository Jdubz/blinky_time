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
      'Fraction of total LEDs used as max active particles (0.1-1.0). Scaled by device size, clamped to pool capacity of 64. Higher = denser effects.',
    unit: '× LEDs',
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
      'Gravity as a fraction of traversal dimension per sec² (negative = upward for fire). Automatically scaled by device height/width.',
    unit: '× traversal/s²',
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
      'Curl-noise turbulence as a fraction of cross dimension per second. Automatically scaled by device width. Higher = stronger lateral swirling.',
    unit: '× cross/s',
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
      'Minimum upward velocity as a fraction of traversal dimension per second. Automatically scaled by device height/width.',
    unit: '× traversal/s',
  },
  sparkvelmax: {
    displayName: 'Max Spark Velocity',
    tooltip:
      'Maximum upward velocity as a fraction of traversal dimension per second. Automatically scaled by device height/width.',
    unit: '× traversal/s',
  },
  sparkspread: {
    displayName: 'Spark Spread',
    tooltip:
      'Horizontal velocity variation as a fraction of cross dimension. Automatically scaled by device width.',
    unit: '× cross/s',
  },
  burstsparks: {
    displayName: 'Burst Spark Count',
    tooltip:
      'Burst sparks as a fraction of cross dimension. Automatically scaled by device width. More = bigger beat reactions.',
    unit: '× cross',
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
      'Thermal buoyancy as a fraction of traversal dimension per sec². Automatically scaled by device height/width. Particles over hot regions are pushed upward.',
    unit: '× traversal/s²',
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
  // WATER GENERATOR (Particle-based, w_ prefix)
  // ============================================================
  w_spawnchance: {
    displayName: 'Spawn Chance',
    tooltip: 'Baseline probability of spawning drops per frame (0-1). Higher = more active rain.',
    unit: '',
  },
  w_audioboost: {
    displayName: 'Audio Boost',
    tooltip: 'Audio reactivity multiplier (0-2). Amplifies spawn rate based on energy level.',
    unit: '',
  },
  w_gravity: {
    displayName: 'Gravity',
    tooltip:
      'Gravity as a fraction of traversal dimension per sec² (positive = downward). Automatically scaled by device height/width.',
    unit: '× traversal/s²',
  },
  w_windbase: {
    displayName: 'Base Wind',
    tooltip: 'Sustained directional drift force (LEDs/sec²).',
    unit: 'LEDs/sec²',
  },
  w_windvar: {
    displayName: 'Wind Variation',
    tooltip:
      'Wind variation as a fraction of cross dimension per second. Automatically scaled by device width.',
    unit: '× cross/s',
  },
  w_drag: {
    displayName: 'Drag',
    tooltip: 'Drag coefficient (0.9-1.0). Closer to 1 = less drag.',
    unit: '',
  },
  w_dropvelmin: {
    displayName: 'Min Drop Velocity',
    tooltip:
      'Minimum downward velocity as a fraction of traversal dimension per second. Automatically scaled by device height/width.',
    unit: '× traversal/s',
  },
  w_dropvelmax: {
    displayName: 'Max Drop Velocity',
    tooltip:
      'Maximum downward velocity as a fraction of traversal dimension per second. Automatically scaled by device height/width.',
    unit: '× traversal/s',
  },
  w_dropspread: {
    displayName: 'Drop Spread',
    tooltip:
      'Horizontal velocity variation as a fraction of cross dimension. Automatically scaled by device width.',
    unit: '× cross/s',
  },
  w_splashparts: {
    displayName: 'Splash Particle Count',
    tooltip:
      'Splash particles as a fraction of cross dimension. Automatically scaled by device width. More = bigger splashes.',
    unit: '× cross',
  },
  w_splashvelmin: {
    displayName: 'Min Splash Velocity',
    tooltip:
      'Minimum splash velocity as a fraction of traversal dimension per second. Automatically scaled by device height/width.',
    unit: '× traversal/s',
  },
  w_splashvelmax: {
    displayName: 'Max Splash Velocity',
    tooltip:
      'Maximum splash velocity as a fraction of traversal dimension per second. Automatically scaled by device height/width.',
    unit: '× traversal/s',
  },
  w_splashint: {
    displayName: 'Splash Intensity',
    tooltip: 'Splash particle intensity multiplier (0-255). Higher = brighter splashes.',
    unit: '',
  },
  w_maxparts: {
    displayName: 'Max Particles',
    tooltip:
      'Fraction of total LEDs used as max active particles (0.1-1.0). Scaled by device size, clamped to pool capacity of 30.',
    unit: '× LEDs',
  },
  w_lifespan: {
    displayName: 'Lifespan',
    tooltip: 'Default particle lifespan in centiseconds (20-180).',
    unit: 'cs',
  },
  w_intmin: {
    displayName: 'Min Intensity',
    tooltip: 'Minimum spawn intensity/brightness (0-255).',
    unit: '',
  },
  w_intmax: {
    displayName: 'Max Intensity',
    tooltip: 'Maximum spawn intensity/brightness (0-255).',
    unit: '',
  },
  w_musicpulse: {
    displayName: 'Music Pulse',
    tooltip: 'Phase modulation for spawn rate (0-1). Higher = stronger beat sync.',
    unit: '',
  },
  w_transmin: {
    displayName: 'Transient Min',
    tooltip: 'Min transient level to trigger burst (0-1). Higher = requires stronger hits.',
    unit: '',
  },
  w_bgintensity: {
    displayName: 'Background Intensity',
    tooltip: 'Noise background brightness (0-1).',
    unit: '',
  },

  // ============================================================
  // LIGHTNING GENERATOR (Particle-based, l_ prefix)
  // ============================================================
  l_spawnchance: {
    displayName: 'Spawn Chance',
    tooltip: 'Baseline probability of spawning bolts per frame (0-1).',
    unit: '',
  },
  l_audioboost: {
    displayName: 'Audio Boost',
    tooltip: 'Audio reactivity multiplier (0-2).',
    unit: '',
  },
  l_faderate: {
    displayName: 'Fade Rate',
    tooltip: 'Intensity decay per frame (0-255). Higher = faster fade, shorter bolts.',
    unit: '',
  },
  l_branchchance: {
    displayName: 'Branch Chance',
    tooltip: 'Branch probability per step (0-100%).',
    unit: '%',
  },
  l_branchcount: {
    displayName: 'Branch Count',
    tooltip: 'Number of branches spawned per trigger (1-4). More = bushier lightning.',
    unit: 'branches',
  },
  l_branchspread: {
    displayName: 'Branch Angle Spread',
    tooltip: 'Angle variation for branches (radians). Higher = wider forking.',
    unit: 'rad',
  },
  l_branchintloss: {
    displayName: 'Branch Intensity Loss',
    tooltip: 'Intensity reduction for child branches (0-100%). Higher = dimmer branches.',
    unit: '%',
  },
  l_maxparts: {
    displayName: 'Max Particles',
    tooltip:
      'Fraction of total LEDs used as max active particles (0.1-1.0). Scaled by device size, clamped to pool capacity of 40.',
    unit: '× LEDs',
  },
  l_lifespan: {
    displayName: 'Lifespan',
    tooltip: 'Default particle lifespan in centiseconds (10-60).',
    unit: 'cs',
  },
  l_intmin: {
    displayName: 'Min Intensity',
    tooltip: 'Minimum spawn intensity/brightness (0-255).',
    unit: '',
  },
  l_intmax: {
    displayName: 'Max Intensity',
    tooltip: 'Maximum spawn intensity/brightness (0-255).',
    unit: '',
  },
  l_musicpulse: {
    displayName: 'Music Pulse',
    tooltip: 'Phase modulation for spawn rate (0-1). Higher = stronger beat sync.',
    unit: '',
  },
  l_transmin: {
    displayName: 'Transient Min',
    tooltip: 'Min transient level to trigger burst (0-1). Higher = requires stronger hits.',
    unit: '',
  },
  l_bgintensity: {
    displayName: 'Background Intensity',
    tooltip: 'Noise background brightness (0-1).',
    unit: '',
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

  // ============================================================
  // TRACKER SETTINGS (AudioTracker — ACF+Comb+PLL, v74-v76)
  // ============================================================

  // Tempo range
  bpmmin: {
    displayName: 'Minimum BPM',
    tooltip: 'Minimum detectable BPM (40-120). Lower bound of tempo detection range. Default: 60.',
    unit: 'BPM',
  },
  bpmmax: {
    displayName: 'Maximum BPM',
    tooltip:
      'Maximum detectable BPM (120-240). Upper bound of tempo detection range. Default: 200.',
    unit: 'BPM',
  },
  rayleighbpm: {
    displayName: 'Rayleigh Prior BPM',
    tooltip:
      'Rayleigh prior peak BPM (60-180). Perceptual bias toward preferred tempo — ACF peaks near this BPM are weighted higher. Default: 130.',
    unit: 'BPM',
  },

  // Comb filter bank
  combfeedback: {
    displayName: 'Comb Feedback',
    tooltip:
      'Comb bank resonance strength (0.85-0.98). Higher = sharper tempo peaks but slower adaptation. Lower = broader peaks, faster response. Default: 0.855.',
    unit: '',
  },

  // Rhythm activation
  activationthreshold: {
    displayName: 'Activation Threshold',
    tooltip:
      'Minimum periodicity strength to activate rhythm mode (0-1). Below this, no rhythm-aware modulation. Default: 0.3.',
    unit: '',
  },
  // Tempo smoothing
  temposmooth: {
    displayName: 'Tempo Smoothing',
    tooltip:
      'BPM EMA smoothing factor (0.5-0.99). Higher = slower BPM changes (more stable), lower = more responsive. Default: 0.85.',
    unit: '',
  },

  // PLP (Predominant Local Pulse) settings
  plpactivation: {
    displayName: 'PLP Activation',
    tooltip:
      'Min PLP confidence for pattern pulse (0-1). Below this threshold, falls back to cosine phase pulse. Default: 0.3.',
    unit: '',
  },
  plpconfalpha: {
    displayName: 'PLP Confidence Smoothing',
    tooltip:
      'PLP confidence EMA smoothing rate (0.01-0.5). Lower = slower, more stable confidence tracking. Default: 0.15.',
    unit: '',
  },
  plpnovgain: {
    displayName: 'PLP Pattern Contrast',
    tooltip:
      'PLP pattern novelty scaling (0.1-5.0). Values >1 sharpen peaks in the PLP pattern. Default: 1.5.',
    unit: '',
  },

  // NN profiling
  nnprofile: {
    displayName: 'NN Profiling',
    tooltip:
      'Enable NN inference profiling output. Prints timing info to serial console for performance debugging.',
    unit: '',
  },

  // Spectral flux contrast
  odfcontrast: {
    displayName: 'ODF Contrast',
    tooltip:
      'Spectral flux contrast exponent (0.1-4.0). Power-law sharpening applied before ACF/comb analysis. Higher = sharper transient peaks, lower = smoother. Default: 1.25.',
    unit: '',
  },

  // Pulse detection
  pulsethreshmult: {
    displayName: 'Pulse Threshold Mult',
    tooltip:
      'Pulse baseline threshold multiplier (1.0-5.0). ODF must exceed baseline by this factor to fire pulse. Higher = fewer pulses. Default: 2.0.',
    unit: 'x',
  },
  pulseminlevel: {
    displayName: 'Pulse Min Level',
    tooltip:
      'Minimum mic level for pulse detection (0-0.2). Prevents firing pulse on silence/noise. Default: 0.03.',
    unit: '',
  },

  // Percival ACF harmonic enhancement
  percival2: {
    displayName: 'Percival 2nd Harmonic',
    tooltip:
      'ACF 2nd harmonic fold weight (0-1). Enhances half-tempo peaks in autocorrelation (Percival method). Default: 0.5.',
    unit: '',
  },
  percival4: {
    displayName: 'Percival 4th Harmonic',
    tooltip:
      'ACF 4th harmonic fold weight (0-1). Enhances quarter-tempo peaks in autocorrelation. Default: 0.25.',
    unit: '',
  },

  // ODF baseline tracking
  blfastdrop: {
    displayName: 'Baseline Fast Drop',
    tooltip:
      'ODF baseline fast drop rate (0.01-0.2). How quickly the floor-tracking baseline drops to follow decreasing ODF. Default: 0.05.',
    unit: '',
  },
  blslowrise: {
    displayName: 'Baseline Slow Rise',
    tooltip:
      'ODF baseline slow rise rate (0.001-0.05). How slowly the baseline rises, preventing it from chasing transients. Default: 0.005.',
    unit: '',
  },
  odfpkdecay: {
    displayName: 'ODF Peak-Hold Decay',
    tooltip:
      'ODF peak-hold release rate (0.5-0.99). Peak-hold value decays at this rate per frame (~100ms at 62.5Hz). Used for energy synthesis. Default: 0.85.',
    unit: '',
  },

  // Energy synthesis
  emicweight: {
    displayName: 'Energy: Mic Weight',
    tooltip:
      'Broadband mic level weight in energy synthesis (0-1). Blend weights should sum to ~1.0. Default: 0.30.',
    unit: '',
  },
  emelweight: {
    displayName: 'Energy: Mel Weight',
    tooltip:
      'Bass mel energy weight in energy synthesis (0-1). Low-frequency mel bands contribution. Default: 0.30.',
    unit: '',
  },
  eodfweight: {
    displayName: 'Energy: ODF Weight',
    tooltip:
      'ODF peak-hold weight in energy synthesis (0-1). Transient contribution to energy. Default: 0.40.',
    unit: '',
  },
  // Spectral flux band weights
  bassflux: {
    displayName: 'Bass Flux Weight',
    tooltip:
      'Spectral flux: bass band weight 62-375 Hz (0-1). Contribution of bass frequencies to the combined spectral flux ODF. Default: 1.0.',
    unit: '',
  },
  midflux: {
    displayName: 'Mid Flux Weight',
    tooltip:
      'Spectral flux: mid band weight 437-2000 Hz (0-1). Contribution of mid frequencies to the combined spectral flux ODF. Default: 1.0.',
    unit: '',
  },
  highflux: {
    displayName: 'High Flux Weight',
    tooltip:
      'Spectral flux: high band weight 2-8 kHz (0-1). Contribution of high frequencies to the combined spectral flux ODF. Default: 1.0.',
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
