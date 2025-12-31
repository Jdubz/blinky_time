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

  // Water generator settings
  waterflow: {
    displayName: 'Base Flow Rate',
    tooltip: 'Base speed of water flow animation. Higher values create faster flowing water.',
    unit: '',
  },
  wavemin: {
    displayName: 'Min Wave Height',
    tooltip: 'Minimum height for generated waves. Lower values allow smaller, subtler ripples.',
    unit: '',
  },
  wavemax: {
    displayName: 'Max Wave Height',
    tooltip:
      'Maximum height for generated waves. Higher values create taller, more dramatic waves.',
    unit: '',
  },
  wavechance: {
    displayName: 'Wave Probability',
    tooltip:
      'Base probability of new waves appearing (0-100%). Higher values create more wave activity.',
    unit: '%',
  },
  audiowaveboost: {
    displayName: 'Audio Wave Boost',
    tooltip:
      'How much audio influences wave generation. Higher values make water more reactive to sound.',
    unit: '%',
  },
  audioflowboostmax: {
    displayName: 'Max Audio Flow Boost',
    tooltip:
      'Maximum flow speed increase from audio. Limits how much sound can accelerate the water.',
    unit: '',
  },
  flowaudiobias: {
    displayName: 'Flow Audio Bias',
    tooltip:
      'How audio affects flow rate. Positive values speed up flow with sound, negative slows it.',
    unit: '',
  },

  // Lightning generator settings
  lightningfade: {
    displayName: 'Fade Speed',
    tooltip:
      'How quickly lightning bolts fade out. Higher values create shorter, snappier strikes.',
    unit: '',
  },
  boltintensitymin: {
    displayName: 'Min Bolt Intensity',
    tooltip:
      'Minimum brightness for lightning bolts. Lower values allow dimmer, distant-looking strikes.',
    unit: '',
  },
  boltintensitymax: {
    displayName: 'Max Bolt Intensity',
    tooltip: 'Maximum brightness for lightning bolts. Higher values create more brilliant strikes.',
    unit: '',
  },
  boltchance: {
    displayName: 'Bolt Probability',
    tooltip:
      'Base probability of new lightning strikes (0-100%). Higher values create more frequent storms.',
    unit: '%',
  },
  audioboltboost: {
    displayName: 'Audio Bolt Boost',
    tooltip:
      'How much audio influences bolt generation. Higher values make lightning more reactive to sound.',
    unit: '%',
  },
  audiointensityboostmax: {
    displayName: 'Max Audio Intensity Boost',
    tooltip:
      'Maximum intensity increase from audio. Limits how bright audio-triggered bolts can get.',
    unit: '',
  },
  fadeaudiobias: {
    displayName: 'Fade Audio Bias',
    tooltip:
      'How audio affects fade rate. Negative values slow fade during loud audio (bolts persist longer).',
    unit: '',
  },
  branchchance: {
    displayName: 'Branch Probability',
    tooltip:
      'Chance of lightning bolts branching (0-100%). Higher values create more complex, forked lightning.',
    unit: '%',
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
