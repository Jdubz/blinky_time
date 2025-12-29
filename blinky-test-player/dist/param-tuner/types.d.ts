/**
 * Types for parameter tuning system
 */
export type DetectionMode = 'drummer' | 'spectral' | 'hybrid' | 'bass' | 'hfc';
export type SubsystemMode = 'music' | 'rhythm';
export type ParameterMode = DetectionMode | SubsystemMode;
export declare const DETECTION_MODES: DetectionMode[];
export declare const SUBSYSTEM_MODES: SubsystemMode[];
export declare const ALL_MODES: ParameterMode[];
export declare const MODE_IDS: Record<DetectionMode, number>;
export interface ParameterDef {
    name: string;
    mode: ParameterMode;
    min: number;
    max: number;
    default: number;
    sweepValues: number[];
    description: string;
}
export declare const PARAMETERS: Record<string, ParameterDef>;
export declare const REPRESENTATIVE_PATTERNS: readonly ["strong-beats", "sparse", "bass-line", "synth-stabs", "pad-rejection", "simultaneous", "hat-rejection", "full-mix"];
export declare const ALL_PATTERNS: readonly ["strong-beats", "medium-beats", "soft-beats", "hat-rejection", "mixed-dynamics", "tempo-sweep", "bass-line", "synth-stabs", "lead-melody", "pad-rejection", "chord-rejection", "full-mix", "basic-drums", "kick-focus", "snare-focus", "hat-patterns", "full-kit", "simultaneous", "fast-tempo", "sparse"];
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
export interface TuningState {
    lastUpdated: string;
    currentPhase: 'baseline' | 'sweep' | 'interact' | 'validate' | 'report' | 'done';
    phasesCompleted: string[];
    baseline?: {
        completed: DetectionMode[];
        current?: DetectionMode;
        results: Record<DetectionMode, BaselineResult>;
    };
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
    validation?: {
        completed: DetectionMode[];
        current?: DetectionMode;
        results: Record<DetectionMode, ValidationResult>;
    };
    optimalParams?: {
        drummer: Record<string, number>;
        bass: Record<string, number>;
        hfc: Record<string, number>;
        spectral: Record<string, number>;
        hybrid: Record<string, number>;
    };
}
export interface TunerOptions {
    port: string;
    gain?: number;
    outputDir?: string;
    params?: string[];
    modes?: ParameterMode[];
    patterns?: string[];
}
