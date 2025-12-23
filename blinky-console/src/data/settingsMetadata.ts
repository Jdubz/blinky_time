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
    displayName: 'Transient Heat Boost',
    tooltip:
      'Maximum heat added on percussive hits/beats. Uses Transient (red spikes) from AdaptiveMic. Higher values make fire "jump" on drum hits.',
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
    tooltip:
      'Minimum audio level to register (0-100%). Controls Level (orange area) in AdaptiveMic visualizer. Filters out background noise and hiss.',
    unit: '%',
  },
  attack: {
    displayName: 'Attack Time',
    tooltip:
      'How quickly envelope responds to sudden increases in volume (seconds). Controls Envelope (blue dashed line) in AdaptiveMic visualizer. Lower = more responsive.',
    unit: 's',
  },
  release: {
    displayName: 'Release Time',
    tooltip:
      'How quickly envelope decays after sound stops (seconds). Controls Envelope (blue dashed line) in AdaptiveMic visualizer. Higher = longer sustain.',
    unit: 's',
  },
  transientcooldown: {
    displayName: 'Transient Cooldown',
    tooltip:
      'Minimum time between transient detections (milliseconds). Controls Transient (red spikes) in AdaptiveMic visualizer. Prevents retriggering on same hit.',
    unit: 'ms',
  },
  transientfactor: {
    displayName: 'Transient Sensitivity',
    tooltip:
      'How sensitive transient detection is. Controls Transient (red spikes) in AdaptiveMic visualizer. Higher values detect subtler attacks/beats.',
    unit: 'x',
  },

  // AGC settings
  agenabled: {
    displayName: 'Auto-Gain Enabled',
    tooltip:
      'Enable automatic gain control to normalize quiet and loud audio sources. Controls Gain value shown in AdaptiveMic visualizer.',
    unit: '',
  },
  agtarget: {
    displayName: 'AGC Target Level',
    tooltip:
      'Target audio level for AGC to maintain (0-100%). AGC adjusts Gain to bring Envelope (blue dashed line) to this level. Produces Level (orange area) output.',
    unit: '%',
  },
  agstrength: {
    displayName: 'AGC Responsiveness',
    tooltip:
      'How quickly AGC adapts to level changes (0-100%). Controls how fast Gain value changes. Higher = faster adaptation.',
    unit: '%',
  },
  agmin: {
    displayName: 'Min AGC Gain',
    tooltip:
      'Minimum gain multiplier. Lower bound for Gain value shown in AdaptiveMic visualizer. Prevents over-attenuation of loud sources.',
    unit: 'x',
  },
  agmax: {
    displayName: 'Max AGC Gain',
    tooltip:
      'Maximum gain multiplier. Upper bound for Gain value shown in AdaptiveMic visualizer. Prevents over-amplification of quiet sources.',
    unit: 'x',
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
    displayName: 'Transient',
    tooltip: 'Percussion/attack detection (0-1). Spikes to 1.0 on drum hits and sharp sounds.',
    unit: '',
  },
  e: {
    displayName: 'Envelope',
    tooltip: 'Smoothed audio envelope (0-1). Uses attack/release times for gradual changes.',
    unit: '',
  },
  g: {
    displayName: 'AGC Gain',
    tooltip: 'Current auto-gain multiplier (1-20x). Shows how much the AGC is boosting the signal.',
    unit: 'x',
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
