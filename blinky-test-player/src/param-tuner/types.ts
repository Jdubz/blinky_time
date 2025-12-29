/**
 * Types for parameter tuning system
 */

// Detection modes
export type DetectionMode = 'drummer' | 'spectral' | 'hybrid' | 'music';
export const DETECTION_MODES: DetectionMode[] = ['drummer', 'spectral', 'hybrid'];
export const MUSIC_MODE = 'music' as const;  // BPM tracking mode (not detection)

// Mode IDs as they appear in device settings
export const MODE_IDS: Record<DetectionMode, number> = {
  drummer: 0,
  spectral: 3,
  hybrid: 4,
  music: -1,  // Not a detection mode - used for BPM tracking params
};

// Parameter definitions with ranges
export interface ParameterDef {
  name: string;
  mode: DetectionMode;
  min: number;
  max: number;
  default: number;
  sweepValues: number[];
  description: string;
}

export const PARAMETERS: Record<string, ParameterDef> = {
  // Drummer parameters
  hitthresh: {
    name: 'hitthresh',
    mode: 'drummer',
    min: 1.5,
    max: 10.0,
    default: 3.0,
    sweepValues: [1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 7.0, 10.0],
    description: 'Main detection threshold',
  },
  attackmult: {
    name: 'attackmult',
    mode: 'drummer',
    min: 1.1,
    max: 2.0,
    default: 1.3,
    sweepValues: [1.1, 1.2, 1.3, 1.4, 1.5, 1.7, 2.0],
    description: 'Attack sensitivity multiplier',
  },
  avgtau: {
    name: 'avgtau',
    mode: 'drummer',
    min: 0.1,
    max: 5.0,
    default: 0.8,
    sweepValues: [0.1, 0.3, 0.5, 0.8, 1.0, 1.5, 2.0, 3.0, 5.0],
    description: 'Envelope smoothing time constant',
  },
  cooldown: {
    name: 'cooldown',
    mode: 'drummer',
    min: 20,
    max: 500,
    default: 80,
    sweepValues: [20, 40, 60, 80, 100, 150, 200, 300, 500],
    description: 'Minimum ms between detections',
  },

  // Spectral Flux parameters
  fluxthresh: {
    name: 'fluxthresh',
    mode: 'spectral',
    min: 1.0,
    max: 10.0,
    default: 3.0,
    sweepValues: [1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 7.0, 10.0],
    description: 'Spectral flux threshold',
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
    default: 0.7,
    sweepValues: [0.1, 0.3, 0.5, 0.7, 0.9, 1.0],
    description: 'Weight for spectral flux component',
  },
  hydrumwt: {
    name: 'hydrumwt',
    mode: 'hybrid',
    min: 0.1,
    max: 1.0,
    default: 0.5,
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

  // ===== MusicMode Parameters (BPM Tracking) =====

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
  mode: DetectionMode;
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
    spectral: Record<string, number>;
    hybrid: Record<string, number>;
  };
}

// CLI options
export interface TunerOptions {
  port: string;
  gain?: number;
  outputDir?: string;
}
