/**
 * Types for parameter tuning system
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy detection mode switching has been removed.
 */
export type DetectorType = 'drummer' | 'spectral' | 'hfc' | 'bass' | 'complex' | 'mel';
export type SubsystemMode = 'music' | 'rhythm';
export type ParameterMode = 'ensemble' | SubsystemMode;
export declare const DETECTOR_TYPES: DetectorType[];
export declare const SUBSYSTEM_MODES: SubsystemMode[];
export declare const ALL_MODES: ParameterMode[];
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
export declare function generateSweepValues(param: ParameterDef): number[];
export declare const PARAMETERS: Record<string, ParameterDef>;
export declare const REPRESENTATIVE_PATTERNS: readonly ["strong-beats", "sparse", "bass-line", "synth-stabs", "pad-rejection", "simultaneous", "hat-rejection", "full-mix"];
export declare const ALL_PATTERNS: readonly ["strong-beats", "medium-beats", "soft-beats", "hat-rejection", "mixed-dynamics", "tempo-sweep", "bass-line", "synth-stabs", "lead-melody", "pad-rejection", "chord-rejection", "full-mix", "basic-drums", "kick-focus", "snare-focus", "hat-patterns", "full-kit", "simultaneous", "fast-tempo", "sparse", "cooldown-stress-20ms", "cooldown-stress-40ms", "cooldown-stress-80ms", "threshold-gradient", "attack-sharp", "attack-gradual", "freq-low-only", "freq-high-only", "steady-120bpm", "steady-80bpm", "steady-160bpm", "tempo-ramp", "tempo-sudden", "phase-on-beat", "phase-off-beat", "non-musical-random", "non-musical-clustered", "silence-gaps"];
export type PatternId = (typeof ALL_PATTERNS)[number];
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
export interface TuningState {
    lastUpdated: string;
    currentPhase: 'baseline' | 'sweep' | 'interact' | 'validate' | 'report' | 'done';
    phasesCompleted: string[];
    baseline?: BaselineResult;
    sweeps?: {
        completed: string[];
        current?: string;
        currentIndex?: number;
        results: Record<string, SweepResult>;
    };
    interactions?: {
        completed: string[];
        current?: string;
        currentIndex?: number;
        results: Record<string, InteractionResult>;
    };
    validation?: ValidationResult;
    optimalParams?: Record<string, number>;
}
export interface TunerOptions {
    port: string;
    gain?: number;
    outputDir?: string;
    params?: string[];
    modes?: ParameterMode[];
    patterns?: string[];
    refine?: boolean;
    refinementSteps?: number;
    recordAudio?: boolean;
}
