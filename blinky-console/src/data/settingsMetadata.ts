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
  // Fire settings
  cooling: {
    displayName: 'Base Cooling',
    tooltip:
      'How fast the fire cools down over time. Higher values create faster cooling and shorter flames.',
    unit: '',
  },
  sparkchance: {
    displayName: 'Spark Probability',
    tooltip:
      'Base probability of new sparks appearing (0-100%). Higher values create more active fire.',
    unit: '%',
  },
  sparkheatmin: {
    displayName: 'Min Spark Heat',
    tooltip: 'Minimum heat value for new sparks. Lower values allow dimmer sparks.',
    unit: '',
  },
  sparkheatmax: {
    displayName: 'Max Spark Heat',
    tooltip:
      'Maximum heat value for new sparks. Higher values create brighter, more intense sparks.',
    unit: '',
  },
  audiosparkboost: {
    displayName: 'Audio Spark Boost',
    tooltip:
      'How much audio influences spark generation (0-100%). Uses Level (orange area) from AdaptiveMic. Higher values make fire more reactive to music.',
    unit: '%',
  },
  coolingaudiobias: {
    displayName: 'Audio Cooling Bias',
    tooltip:
      'How audio affects cooling rate. Uses Level (orange area) from AdaptiveMic. Negative values slow cooling during loud audio, positive speeds it up.',
    unit: '',
  },
  bottomrows: {
    displayName: 'Spark Injection Rows',
    tooltip:
      'Number of bottom rows where sparks can appear. More rows create taller initial flames.',
    unit: 'rows',
  },
  burstsparks: {
    displayName: 'Burst Spark Count',
    tooltip:
      'Number of sparks generated during a burst event. More sparks = bigger burst reactions.',
    unit: 'sparks',
  },
  suppressionms: {
    displayName: 'Burst Cooldown',
    tooltip:
      'Time to wait after a burst before allowing another (milliseconds). Prevents excessive bursting.',
    unit: 'ms',
  },
  heatdecay: {
    displayName: 'Heat Decay Factor',
    tooltip:
      'Rate at which heat dissipates over distance. Lower values = faster decay, shorter flames.',
    unit: '',
  },
  emberheatmax: {
    displayName: 'Max Ember Heat',
    tooltip:
      'Maximum heat for ambient ember glow. Creates subtle background glow even without sparks.',
    unit: '',
  },
  spreaddistance: {
    displayName: 'Heat Spread Distance',
    tooltip:
      'How far heat propagates between pixels during each frame. Higher values create longer, more flowing flames.',
    unit: 'pixels',
  },
  embernoisespeed: {
    displayName: 'Ember Animation Speed',
    tooltip: 'Speed of the ember glow animation. Higher values create faster flickering embers.',
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

  // Legacy AGC settings (may be deprecated)
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
