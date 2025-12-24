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
  audioheatboost: {
    displayName: 'Audio Heat Boost',
    tooltip:
      'Maximum additional heat from audio input. Uses Level (orange area) from AdaptiveMic. Higher values create stronger audio-reactive flames.',
    unit: '',
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
  transientheatmax: {
    displayName: 'Percussion Heat Boost',
    tooltip:
      'Maximum heat added on percussion hits (kick/snare/hihat). Higher values make fire "jump" more dramatically on drum hits.',
    unit: '',
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

  // Audio settings
  gate: {
    displayName: 'Noise Gate',
    tooltip: 'Minimum audio level to register (0-100%). Filters out background noise and hiss.',
    unit: '%',
  },
  transientcooldown: {
    displayName: 'Percussion Cooldown',
    tooltip:
      'Minimum time between percussion detections (milliseconds). Prevents retriggering on same kick/snare/hihat hit.',
    unit: 'ms',
  },

  // AGC settings (peak-based design, target always 100%)
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
      'Post-AGC audio level (0-1). This is the final processed amplitude that drives fire intensity.',
    unit: '',
  },
  t: {
    displayName: 'Percussion',
    tooltip:
      'Percussion strength (0-1+). Maximum of kick/snare/hihat detection. Single-frame impulse when percussion detected. Drives fire bursts.',
    unit: '',
  },
  r: {
    displayName: 'RMS Level',
    tooltip:
      'Tracked RMS level (0-1). The average audio level that AGC is targeting. Smoother than instantaneous level.',
    unit: '',
  },
  s: {
    displayName: 'SW Gain',
    tooltip:
      'Software AGC gain multiplier (0.01-100x). Fast-adapting automatic gain control. Works with hardware gain for full dynamic range.',
    unit: 'x',
  },
  h: {
    displayName: 'HW Gain',
    tooltip:
      'Hardware PDM gain setting (0-80). Slow-adapting discrete gain. Adjusts when software gain is pinned at limits for extended periods.',
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
