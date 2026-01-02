/**
 * Types for parameter tuning system
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy detection mode switching has been removed.
 */

// Detector types (all run simultaneously in ensemble)
export type DetectorType = 'drummer' | 'spectral' | 'hfc' | 'bass' | 'complex' | 'mel';

// Subsystem modes for rhythm tracking
export type SubsystemMode = 'music' | 'rhythm';

// Parameter modes (ensemble replaces old detection modes)
export type ParameterMode = 'ensemble' | SubsystemMode;

// All detector types
export const DETECTOR_TYPES: DetectorType[] = ['drummer', 'spectral', 'hfc', 'bass', 'complex', 'mel'];
export const SUBSYSTEM_MODES: SubsystemMode[] = ['music', 'rhythm'];
export const ALL_MODES: ParameterMode[] = ['ensemble', ...SUBSYSTEM_MODES];

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

  /** Serial command to set this parameter (e.g., "detector_thresh drummer") */
  command?: string;

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

  /** Detector this parameter applies to (for ensemble params) */
  detector?: DetectorType;
}

/**
 * Generate sweep values from min/max/step if sweepValues not provided
 * Uses index-based loop to avoid floating-point precision issues
 */
export function generateSweepValues(param: ParameterDef): number[] {
  if (param.sweepValues.length > 0) {
    return param.sweepValues;
  }

  const step = param.step ?? (param.max - param.min) / 10;
  const values: number[] = [];

  // Use index-based loop to avoid floating-point accumulation errors
  for (let i = 0; ; i++) {
    const v = param.min + i * step;
    if (v > param.max + 1e-9) break;  // Small epsilon for floating-point comparison
    values.push(Math.round(v * 1000) / 1000);
  }

  return values;
}

export const PARAMETERS: Record<string, ParameterDef> = {
  // ===== ENSEMBLE DETECTOR THRESHOLDS =====
  // Each detector has its own threshold controlling sensitivity

  drummer_thresh: {
    name: 'drummer_thresh',
    mode: 'ensemble',
    command: 'detector_thresh drummer',
    detector: 'drummer',
    min: 0.5,
    max: 10.0,
    default: 2.5,
    sweepValues: [0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0, 10.0],
    description: 'Drummer detector threshold (amplitude ratio)',
    targetPatternCategories: ['transient', 'parameter-targeted'],
    targetPatterns: ['strong-beats', 'soft-beats', 'threshold-gradient'],
    optimizeFor: 'f1',
  },

  spectral_thresh: {
    name: 'spectral_thresh',
    mode: 'ensemble',
    command: 'detector_thresh spectral',
    detector: 'spectral',
    min: 0.5,
    max: 10.0,
    default: 1.4,
    sweepValues: [0.5, 0.8, 1.0, 1.2, 1.4, 1.6, 2.0, 2.5, 3.0, 5.0],
    description: 'Spectral flux detector threshold',
    targetPatternCategories: ['transient', 'rejection'],
    targetPatterns: ['strong-beats', 'pad-rejection', 'chord-rejection'],
    optimizeFor: 'f1',
  },

  hfc_thresh: {
    name: 'hfc_thresh',
    mode: 'ensemble',
    command: 'detector_thresh hfc',
    detector: 'hfc',
    min: 1.0,
    max: 10.0,
    default: 3.0,
    sweepValues: [1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0, 10.0],
    description: 'High-frequency content detector threshold',
    targetPatternCategories: ['transient'],
    targetPatterns: ['hat-rejection', 'freq-high-only'],
    optimizeFor: 'f1',
  },

  bass_thresh: {
    name: 'bass_thresh',
    mode: 'ensemble',
    command: 'detector_thresh bass',
    detector: 'bass',
    min: 1.0,
    max: 10.0,
    default: 3.0,
    sweepValues: [1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0, 10.0],
    description: 'Bass band detector threshold',
    targetPatternCategories: ['transient'],
    targetPatterns: ['bass-line', 'freq-low-only', 'kick-focus'],
    optimizeFor: 'f1',
  },

  complex_thresh: {
    name: 'complex_thresh',
    mode: 'ensemble',
    command: 'detector_thresh complex',
    detector: 'complex',
    min: 1.0,
    max: 10.0,
    default: 2.0,
    sweepValues: [1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0],
    description: 'Complex domain detector threshold (phase deviation)',
    targetPatternCategories: ['transient'],
    targetPatterns: ['soft-beats', 'attack-gradual', 'lead-melody'],
    optimizeFor: 'f1',
  },

  mel_thresh: {
    name: 'mel_thresh',
    mode: 'ensemble',
    command: 'detector_thresh mel',
    detector: 'mel',
    min: 1.0,
    max: 10.0,
    default: 2.5,
    sweepValues: [1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0],
    description: 'Mel flux detector threshold (perceptual)',
    targetPatternCategories: ['transient'],
    targetPatterns: ['full-mix', 'mixed-dynamics'],
    optimizeFor: 'f1',
  },

  // ===== ENSEMBLE DETECTOR WEIGHTS =====
  // Each detector's contribution to the final ensemble score

  drummer_weight: {
    name: 'drummer_weight',
    mode: 'ensemble',
    command: 'detector_weight drummer',
    detector: 'drummer',
    min: 0.0,
    max: 0.5,
    default: 0.22,
    sweepValues: [0.05, 0.1, 0.15, 0.2, 0.22, 0.25, 0.3, 0.35, 0.4],
    description: 'Drummer detector weight in ensemble',
    optimizeFor: 'f1',
  },

  spectral_weight: {
    name: 'spectral_weight',
    mode: 'ensemble',
    command: 'detector_weight spectral',
    detector: 'spectral',
    min: 0.0,
    max: 0.5,
    default: 0.20,
    sweepValues: [0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4],
    description: 'Spectral flux detector weight in ensemble',
    optimizeFor: 'f1',
  },

  hfc_weight: {
    name: 'hfc_weight',
    mode: 'ensemble',
    command: 'detector_weight hfc',
    detector: 'hfc',
    min: 0.0,
    max: 0.5,
    default: 0.15,
    sweepValues: [0.05, 0.1, 0.15, 0.2, 0.25, 0.3],
    description: 'HFC detector weight in ensemble',
    optimizeFor: 'f1',
  },

  bass_weight: {
    name: 'bass_weight',
    mode: 'ensemble',
    command: 'detector_weight bass',
    detector: 'bass',
    min: 0.0,
    max: 0.5,
    default: 0.18,
    sweepValues: [0.05, 0.1, 0.15, 0.18, 0.2, 0.25, 0.3],
    description: 'Bass band detector weight in ensemble',
    optimizeFor: 'f1',
  },

  complex_weight: {
    name: 'complex_weight',
    mode: 'ensemble',
    command: 'detector_weight complex',
    detector: 'complex',
    min: 0.0,
    max: 0.5,
    default: 0.13,
    sweepValues: [0.05, 0.1, 0.13, 0.15, 0.2, 0.25],
    description: 'Complex domain detector weight in ensemble',
    optimizeFor: 'f1',
  },

  mel_weight: {
    name: 'mel_weight',
    mode: 'ensemble',
    command: 'detector_weight mel',
    detector: 'mel',
    min: 0.0,
    max: 0.5,
    default: 0.12,
    sweepValues: [0.05, 0.1, 0.12, 0.15, 0.2, 0.25],
    description: 'Mel flux detector weight in ensemble',
    optimizeFor: 'f1',
  },

  // ===== AGREEMENT BOOST VALUES =====
  // Confidence scaling based on detector agreement

  agree_1: {
    name: 'agree_1',
    mode: 'ensemble',
    command: 'agree_1',
    min: 0.3,
    max: 0.9,
    default: 0.6,
    sweepValues: [0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9],
    description: 'Boost when single detector fires (suppress false positives)',
    targetPatterns: ['hat-rejection', 'pad-rejection', 'chord-rejection'],
    optimizeFor: 'precision',
  },

  agree_2: {
    name: 'agree_2',
    mode: 'ensemble',
    command: 'agree_2',
    min: 0.6,
    max: 1.0,
    default: 0.85,
    sweepValues: [0.6, 0.7, 0.8, 0.85, 0.9, 0.95, 1.0],
    description: 'Boost when two detectors agree',
    optimizeFor: 'f1',
  },

  agree_3: {
    name: 'agree_3',
    mode: 'ensemble',
    command: 'agree_3',
    min: 0.8,
    max: 1.2,
    default: 1.0,
    sweepValues: [0.8, 0.9, 1.0, 1.05, 1.1, 1.15, 1.2],
    description: 'Boost when three detectors agree',
    optimizeFor: 'f1',
  },

  agree_4: {
    name: 'agree_4',
    mode: 'ensemble',
    command: 'agree_4',
    min: 0.9,
    max: 1.3,
    default: 1.1,
    sweepValues: [0.9, 1.0, 1.05, 1.1, 1.15, 1.2, 1.3],
    description: 'Boost when four detectors agree',
    optimizeFor: 'f1',
  },

  agree_5: {
    name: 'agree_5',
    mode: 'ensemble',
    command: 'agree_5',
    min: 1.0,
    max: 1.4,
    default: 1.15,
    sweepValues: [1.0, 1.05, 1.1, 1.15, 1.2, 1.3, 1.4],
    description: 'Boost when five detectors agree',
    optimizeFor: 'recall',
  },

  agree_6: {
    name: 'agree_6',
    mode: 'ensemble',
    command: 'agree_6',
    min: 1.0,
    max: 1.5,
    default: 1.2,
    sweepValues: [1.0, 1.1, 1.15, 1.2, 1.3, 1.4, 1.5],
    description: 'Boost when all six detectors agree',
    optimizeFor: 'recall',
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

// Baseline result for ensemble (replaces per-mode baselines)
export interface BaselineResult {
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
  baseline?: BaselineResult;

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
  validation?: ValidationResult;

  // Optimal parameters found
  optimalParams?: Record<string, number>;
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
