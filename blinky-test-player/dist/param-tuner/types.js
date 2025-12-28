/**
 * Types for parameter tuning system
 */
export const DETECTION_MODES = ['drummer', 'spectral', 'hybrid'];
// Mode IDs as they appear in device settings
export const MODE_IDS = {
    drummer: 0,
    spectral: 3,
    hybrid: 4,
};
export const PARAMETERS = {
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
};
// Pattern categories for testing
// Representative patterns for quick sweeps (8 patterns covering key scenarios)
export const REPRESENTATIVE_PATTERNS = [
    'strong-beats', // Baseline - hard hits
    'sparse', // Long gaps between hits
    'bass-line', // Low frequency transients
    'synth-stabs', // Sharp melodic transients
    'pad-rejection', // False positive rejection
    'simultaneous', // Concurrent low+high hits
    'hat-rejection', // Soft hi-hat rejection
    'full-mix', // Complex layered audio
];
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
];
