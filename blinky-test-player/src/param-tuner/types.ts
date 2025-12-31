/**
 * Types for parameter tuning system
 */

// Detection modes and subsystems
export type DetectionMode = 'drummer' | 'spectral' | 'hybrid' | 'bass' | 'hfc';
export type SubsystemMode = 'music' | 'rhythm';
export type ParameterMode = DetectionMode | SubsystemMode;

export const DETECTION_MODES: DetectionMode[] = ['drummer', 'spectral', 'hybrid', 'bass', 'hfc'];
export const SUBSYSTEM_MODES: SubsystemMode[] = ['music', 'rhythm'];
export const ALL_MODES: ParameterMode[] = [...DETECTION_MODES, ...SUBSYSTEM_MODES];

// Mode IDs as they appear in device settings (detection modes only)
export const MODE_IDS: Record<DetectionMode, number> = {
  drummer: 0,
  bass: 1,
  hfc: 2,
  spectral: 3,
  hybrid: 4,
};

// Import pattern types for extensibility
import type { PatternCategory, OptimizationMetric } from '../types.js';

/**
 * Parameter definition with extensibility fields
 *
 * EXTENSIBILITY: Adding a new parameter requires only:
 * 1. Add entry to PARAMETERS with targetPatterns
 * 2. System auto-selects relevant patterns for sweeps
 */
export interface ParameterDef {
  name: string;
  mode: ParameterMode;
  min: number;
  max: number;
  default: number;

  /** Step size for auto-generating sweep values (optional) */
  step?: number;

  /** Manual sweep values (takes precedence over step-generated) */
  sweepValues: number[];

  description: string;

  // EXTENSIBILITY FIELDS

  /** Pattern categories relevant for testing this parameter */
  targetPatternCategories?: PatternCategory[];

  /** Specific pattern IDs that test this parameter */
  targetPatterns?: string[];

  /** Primary metric to optimize for this parameter */
  optimizeFor?: OptimizationMetric;
}

/**
 * Generate sweep values from min/max/step if sweepValues not provided
 */
export function generateSweepValues(param: ParameterDef): number[] {
  if (param.sweepValues.length > 0) {
    return param.sweepValues;
  }
  if (!param.step) {
    // Default: 10 steps between min and max
    const step = (param.max - param.min) / 10;
    const values: number[] = [];
    for (let v = param.min; v <= param.max; v += step) {
      values.push(Math.round(v * 1000) / 1000);
    }
    return values;
  }
  const values: number[] = [];
  for (let v = param.min; v <= param.max; v += param.step) {
    values.push(Math.round(v * 1000) / 1000);
  }
  return values;
}

export const PARAMETERS: Record<string, ParameterDef> = {
  // Drummer parameters
  hitthresh: {
    name: 'hitthresh',
    mode: 'drummer',
    min: 0.5,  // Extended from 1.0 (2025-12-30: optimal 1.192 was near boundary, needs retest)
    max: 10.0,
    default: 1.688,  // Fast-tune optimal (was 2.0)
    sweepValues: [0.5, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.4, 1.5, 1.688, 2.0, 2.5, 3.0, 4.0, 5.0],
    description: 'Main detection threshold',
    // Extensibility: patterns that test this parameter
    targetPatternCategories: ['transient', 'parameter-targeted'],
    targetPatterns: ['strong-beats', 'soft-beats', 'threshold-gradient'],
    optimizeFor: 'f1',
  },
  attackmult: {
    name: 'attackmult',
    mode: 'drummer',
    min: 0.9,  // Extended from 1.0 (2025-12-30: optimal 1.1 still near boundary, needs retest)
    max: 2.0,
    default: 1.1,  // Fast-tune optimal (was 1.3)
    sweepValues: [0.9, 0.95, 1.0, 1.05, 1.1, 1.2, 1.3, 1.4, 1.5, 1.7, 2.0],
    description: 'Attack sensitivity multiplier',
    targetPatternCategories: ['transient', 'parameter-targeted'],
    targetPatterns: ['strong-beats', 'attack-sharp', 'attack-gradual'],
    optimizeFor: 'f1',
  },
  avgtau: {
    name: 'avgtau',
    mode: 'drummer',
    min: 0.1,
    max: 5.0,
    default: 0.8,  // Confirmed optimal
    sweepValues: [0.1, 0.3, 0.5, 0.8, 1.0, 1.5, 2.0, 3.0, 5.0],
    description: 'Envelope smoothing time constant',
  },
  cooldown: {
    name: 'cooldown',
    mode: 'drummer',
    min: 20,
    max: 500,
    default: 40,  // Fast-tune optimal (was 80)
    sweepValues: [20, 40, 60, 80, 100, 150, 200, 300, 500],
    description: 'Minimum ms between detections',
    targetPatternCategories: ['transient', 'parameter-targeted'],
    targetPatterns: ['fast-tempo', 'simultaneous', 'cooldown-stress-20ms', 'cooldown-stress-40ms'],
    optimizeFor: 'f1',
  },

  // Spectral Flux parameters
  fluxthresh: {
    name: 'fluxthresh',
    mode: 'spectral',
    min: 0.5,  // Extended from 1.0 (optimal 1.4 was near boundary)
    max: 10.0,
    default: 1.4,  // Fast-tune optimal (was 2.8)
    sweepValues: [0.5, 0.8, 1.0, 1.2, 1.4, 1.5, 2.0, 2.5, 2.8, 3.0, 4.0, 5.0, 7.0, 10.0],
    description: 'Spectral flux threshold',
    targetPatternCategories: ['transient', 'rejection'],
    targetPatterns: ['strong-beats', 'pad-rejection', 'chord-rejection', 'threshold-gradient'],
    optimizeFor: 'f1',
  },
  fluxbins: {
    name: 'fluxbins',
    mode: 'spectral',
    min: 4,
    max: 128,
    default: 64,
    sweepValues: [4, 8, 16, 32, 64, 96, 128],
    description: 'Number of FFT bins to analyze',
  },

  // Hybrid parameters
  hyfluxwt: {
    name: 'hyfluxwt',
    mode: 'hybrid',
    min: 0.1,
    max: 1.0,
    default: 0.7,  // Fast-tune confirmed optimal
    sweepValues: [0.1, 0.3, 0.5, 0.7, 0.9, 1.0],
    description: 'Weight for spectral flux component',
  },
  hydrumwt: {
    name: 'hydrumwt',
    mode: 'hybrid',
    min: 0.1,
    max: 1.0,
    default: 0.3,  // Fast-tune optimal (was 0.5)
    sweepValues: [0.1, 0.3, 0.5, 0.7, 0.9, 1.0],
    description: 'Weight for drummer component',
  },
  hybothboost: {
    name: 'hybothboost',
    mode: 'hybrid',
    min: 1.0,
    max: 2.0,
    default: 1.2,
    sweepValues: [1.0, 1.1, 1.2, 1.3, 1.5, 1.7, 2.0],
    description: 'Boost when both algorithms agree',
  },

  // Bass Band mode parameters (mode 1)
  bassfreq: {
    name: 'bassfreq',
    mode: 'bass',
    min: 40.0,
    max: 200.0,
    default: 120.0,
    sweepValues: [40, 60, 80, 100, 120, 150, 180, 200],
    description: 'Bass filter cutoff frequency (Hz)',
  },
  bassq: {
    name: 'bassq',
    mode: 'bass',
    min: 0.5,
    max: 3.0,
    default: 1.0,
    sweepValues: [0.5, 0.7, 1.0, 1.5, 2.0, 2.5, 3.0],
    description: 'Bass filter Q factor',
  },
  bassthresh: {
    name: 'bassthresh',
    mode: 'bass',
    min: 1.5,
    max: 10.0,
    default: 3.0,
    sweepValues: [1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0, 10.0],
    description: 'Bass detection threshold',
  },

  // HFC mode parameters (mode 2)
  hfcweight: {
    name: 'hfcweight',
    mode: 'hfc',
    min: 0.5,
    max: 5.0,
    default: 1.0,
    sweepValues: [0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0],
    description: 'HFC weighting factor',
  },
  hfcthresh: {
    name: 'hfcthresh',
    mode: 'hfc',
    min: 1.5,
    max: 10.0,
    default: 3.0,
    sweepValues: [1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0, 10.0],
    description: 'HFC detection threshold',
  },

  // ===== MusicMode Parameters (BPM Tracking) =====

  // Activation parameters
  musicthresh: {
    name: 'musicthresh',
    mode: 'music',
    min: 0.0,
    max: 1.0,
    default: 0.6,
    sweepValues: [0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9],
    description: 'Music mode activation threshold',
    targetPatternCategories: ['music-mode'],
    targetPatterns: ['steady-120bpm', 'non-musical-random', 'silence-gaps'],
    optimizeFor: 'bpm_accuracy',
  },
  musicbeats: {
    name: 'musicbeats',
    mode: 'music',
    min: 2,
    max: 16,
    default: 4,
    sweepValues: [2, 3, 4, 6, 8, 12, 16],
    description: 'Stable beats required to activate',
  },
  musicmissed: {
    name: 'musicmissed',
    mode: 'music',
    min: 4,
    max: 16,
    default: 8,
    sweepValues: [4, 6, 8, 10, 12, 16],
    description: 'Missed beats before deactivation',
  },

  // Phase snap tuning
  phasesnap: {
    name: 'phasesnap',
    mode: 'music',
    min: 0.1,
    max: 0.5,
    default: 0.3,
    sweepValues: [0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45],
    description: 'Phase error threshold for snap vs gradual correction',
  },
  snapconf: {
    name: 'snapconf',
    mode: 'music',
    min: 0.1,
    max: 0.8,
    default: 0.4,
    sweepValues: [0.2, 0.3, 0.4, 0.5, 0.6, 0.7],
    description: 'Confidence below this enables phase snap',
  },
  stablephase: {
    name: 'stablephase',
    mode: 'music',
    min: 0.1,
    max: 0.4,
    default: 0.2,
    sweepValues: [0.1, 0.15, 0.2, 0.25, 0.3, 0.35],
    description: 'Phase error below this counts as stable beat',
  },

  // Confidence tuning
  confinc: {
    name: 'confinc',
    mode: 'music',
    min: 0.01,
    max: 0.3,
    default: 0.1,
    sweepValues: [0.03, 0.05, 0.08, 0.1, 0.15, 0.2, 0.25],
    description: 'Confidence gained per stable beat',
  },
  confdec: {
    name: 'confdec',
    mode: 'music',
    min: 0.01,
    max: 0.3,
    default: 0.1,
    sweepValues: [0.03, 0.05, 0.08, 0.1, 0.15, 0.2, 0.25],
    description: 'Confidence lost per unstable beat',
  },
  misspenalty: {
    name: 'misspenalty',
    mode: 'music',
    min: 0.01,
    max: 0.2,
    default: 0.05,
    sweepValues: [0.02, 0.03, 0.05, 0.08, 0.1, 0.15],
    description: 'Confidence lost per missed beat',
  },

  // BPM range
  bpmmin: {
    name: 'bpmmin',
    mode: 'music',
    min: 40.0,
    max: 120.0,
    default: 60.0,
    sweepValues: [40, 50, 60, 70, 80, 100, 120],
    description: 'Minimum BPM',
  },
  bpmmax: {
    name: 'bpmmax',
    mode: 'music',
    min: 120.0,
    max: 240.0,
    default: 200.0,
    sweepValues: [120, 140, 160, 180, 200, 220, 240],
    description: 'Maximum BPM',
  },

  // PLL tuning
  pllkp: {
    name: 'pllkp',
    mode: 'music',
    min: 0.01,
    max: 0.5,
    default: 0.1,
    sweepValues: [0.01, 0.05, 0.1, 0.15, 0.2, 0.3, 0.5],
    description: 'PLL proportional gain',
  },
  pllki: {
    name: 'pllki',
    mode: 'music',
    min: 0.001,
    max: 0.1,
    default: 0.01,
    sweepValues: [0.001, 0.005, 0.01, 0.02, 0.05, 0.1],
    description: 'PLL integral gain',
  },

  // Tempo estimation (comb filter)
  combdecay: {
    name: 'combdecay',
    mode: 'music',
    min: 0.85,
    max: 0.99,
    default: 0.95,
    sweepValues: [0.88, 0.90, 0.92, 0.95, 0.97, 0.98],
    description: 'Comb filter energy decay per frame (higher = more memory)',
  },
  combfb: {
    name: 'combfb',
    mode: 'music',
    min: 0.4,
    max: 0.95,
    default: 0.8,
    sweepValues: [0.5, 0.6, 0.7, 0.8, 0.85, 0.9],
    description: 'Comb filter resonance sharpness',
  },
  combconf: {
    name: 'combconf',
    mode: 'music',
    min: 0.2,
    max: 0.8,
    default: 0.5,
    sweepValues: [0.3, 0.4, 0.5, 0.6, 0.7],
    description: 'Comb filters only update BPM below this confidence',
  },
  histblend: {
    name: 'histblend',
    mode: 'music',
    min: 0.05,
    max: 0.5,
    default: 0.2,
    sweepValues: [0.1, 0.15, 0.2, 0.25, 0.3, 0.4],
    description: 'Histogram tempo estimate blend factor',
  },

  // RhythmAnalyzer parameters (autocorrelation-based tempo detection)
  rhythmminbpm: {
    name: 'rhythmminbpm',
    mode: 'rhythm',
    min: 60.0,
    max: 120.0,
    default: 60.0,
    sweepValues: [60, 70, 80, 90, 100, 110, 120],
    description: 'Minimum BPM for autocorrelation',
  },
  rhythmmaxbpm: {
    name: 'rhythmmaxbpm',
    mode: 'rhythm',
    min: 120.0,
    max: 240.0,
    default: 200.0,
    sweepValues: [120, 140, 160, 180, 200, 220, 240],
    description: 'Maximum BPM for autocorrelation',
  },
  rhythminterval: {
    name: 'rhythminterval',
    mode: 'rhythm',
    min: 500,
    max: 2000,
    default: 1000,
    sweepValues: [500, 750, 1000, 1250, 1500, 2000],
    description: 'Autocorrelation update interval (ms)',
  },
  beatthresh: {
    name: 'beatthresh',
    mode: 'rhythm',
    min: 0.5,
    max: 0.9,
    default: 0.7,
    sweepValues: [0.5, 0.6, 0.7, 0.75, 0.8, 0.85, 0.9],
    description: 'Beat likelihood threshold',
  },
  minperiodicity: {
    name: 'minperiodicity',
    mode: 'rhythm',
    min: 0.3,
    max: 0.8,
    default: 0.5,
    sweepValues: [0.3, 0.4, 0.5, 0.6, 0.7, 0.8],
    description: 'Minimum periodicity strength',
  },
};

// Pattern categories for testing
// Representative patterns for quick sweeps (8 patterns covering key scenarios)
export const REPRESENTATIVE_PATTERNS = [
  'strong-beats',     // Baseline - hard hits
  'sparse',           // Long gaps between hits
  'bass-line',        // Low frequency transients
  'synth-stabs',      // Sharp melodic transients
  'pad-rejection',    // False positive rejection
  'simultaneous',     // Concurrent low+high hits
  'hat-rejection',    // Soft hi-hat rejection
  'full-mix',         // Complex layered audio
] as const;

// All patterns that actually exist in patterns.ts
export const ALL_PATTERNS = [
  // Calibrated patterns (deterministic samples)
  'strong-beats',
  'medium-beats',
  'soft-beats',
  'hat-rejection',
  'mixed-dynamics',
  'tempo-sweep',
  // Melodic/harmonic patterns
  'bass-line',
  'synth-stabs',
  'lead-melody',
  'pad-rejection',
  'chord-rejection',
  'full-mix',
  // Legacy patterns (random samples)
  'basic-drums',
  'kick-focus',
  'snare-focus',
  'hat-patterns',
  'full-kit',
  'simultaneous',
  'fast-tempo',
  'sparse',
  // Parameter-targeted patterns
  'cooldown-stress-20ms',
  'cooldown-stress-40ms',
  'cooldown-stress-80ms',
  'threshold-gradient',
  'attack-sharp',
  'attack-gradual',
  'freq-low-only',
  'freq-high-only',
  // Music mode patterns
  'steady-120bpm',
  'steady-80bpm',
  'steady-160bpm',
  'tempo-ramp',
  'tempo-sudden',
  'phase-on-beat',
  'phase-off-beat',
  'non-musical-random',
  'non-musical-clustered',
  'silence-gaps',
] as const;

export type PatternId = (typeof ALL_PATTERNS)[number];

// Test run result
export interface TestResult {
  pattern: string;
  durationMs: number;
  f1: number;
  precision: number;
  recall: number;
  truePositives: number;
  falsePositives: number;
  falseNegatives: number;
  expectedTotal: number;
  avgTimingErrorMs: number | null;
  audioLatencyMs: number;
}

// Baseline result for one algorithm
export interface BaselineResult {
  algorithm: DetectionMode;
  timestamp: string;
  defaults: Record<string, number>;
  patterns: Record<string, TestResult>;
  overall: {
    avgF1: number;
    avgPrecision: number;
    avgRecall: number;
  };
}

// Sweep result for one parameter
export interface SweepPoint {
  value: number;
  avgF1: number;
  avgPrecision: number;
  avgRecall: number;
  byPattern: Record<string, TestResult>;
}

export interface SweepResult {
  parameter: string;
  mode: ParameterMode;
  timestamp: string;
  sweep: SweepPoint[];
  optimal: {
    value: number;
    avgF1: number;
  };
}

// Interaction test result
export interface InteractionPoint {
  params: Record<string, number>;
  avgF1: number;
  avgPrecision: number;
  avgRecall: number;
  byPattern: Record<string, TestResult>;
}

export interface InteractionResult {
  name: string;
  timestamp: string;
  grid: InteractionPoint[];
  optimal: {
    params: Record<string, number>;
    avgF1: number;
  };
}

// Validation result
export interface ValidationResult {
  algorithm: DetectionMode;
  params: Record<string, number>;
  timestamp: string;
  patterns: Record<string, TestResult>;
  overall: {
    f1: number;
    precision: number;
    recall: number;
  };
  vsBaseline: {
    f1Delta: number;
    improved: string[];
    regressed: string[];
  };
}

// Resume state
export interface TuningState {
  lastUpdated: string;
  currentPhase: 'baseline' | 'sweep' | 'interact' | 'validate' | 'report' | 'done';
  phasesCompleted: string[];

  // Baseline state
  baseline?: {
    completed: DetectionMode[];
    current?: DetectionMode;
    results: Record<DetectionMode, BaselineResult>;
  };

  // Sweep state
  sweeps?: {
    completed: string[];
    current?: string;
    currentIndex?: number;
    results: Record<string, SweepResult>;
  };

  // Interaction state
  interactions?: {
    completed: string[];
    current?: string;
    currentIndex?: number;
    results: Record<string, InteractionResult>;
  };

  // Validation state
  validation?: {
    completed: DetectionMode[];
    current?: DetectionMode;
    results: Record<DetectionMode, ValidationResult>;
  };

  // Optimal parameters found
  optimalParams?: {
    drummer: Record<string, number>;
    bass: Record<string, number>;
    hfc: Record<string, number>;
    spectral: Record<string, number>;
    hybrid: Record<string, number>;
  };
}

// CLI options
export interface TunerOptions {
  port: string;
  gain?: number;
  outputDir?: string;
  params?: string[];  // Optional: specific parameters to tune (defaults to all)
  modes?: ParameterMode[];  // Optional: specific modes to tune (defaults to all)
  patterns?: string[];  // Optional: specific test patterns to use (defaults to all representative patterns)
}
