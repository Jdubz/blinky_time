/**
 * Calibration Test Suite
 * Orchestrates the full calibration workflow
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Single ensemble-based calibration workflow.
 * Legacy per-mode testing has been removed.
 *
 * Workflow:
 * 1. Baseline - Establish baseline performance with defaults
 * 2. Sweep - Find optimal value for each parameter
 * 3. Interact - Test parameter interactions
 * 4. Validate - Validate optimal parameters on all patterns
 * 5. Report - Generate summary report
 */
import type { TunerOptions, ParameterDef } from './types.js';
import { StateManager } from './state.js';
/**
 * Baseline phase configuration
 */
export interface BaselineConfig {
    /** Specific patterns to use (default: auto-select) */
    patterns?: string[];
    /** Number of repetitions per pattern (default: 3) */
    repetitions?: number;
}
/**
 * Single parameter sweep configuration
 */
export interface SweepConfigItem {
    /** Parameter name to sweep */
    parameter: string;
    /** Override default sweep values */
    values?: number[];
    /** Patterns to use for this sweep (default: auto-select by param) */
    patterns?: string[];
}
/**
 * Validation phase configuration
 */
export interface ValidationConfig {
    /** Patterns to use (default: all) */
    patterns?: string[];
    /** Number of repetitions per pattern (default: 3) */
    repetitions?: number;
}
/**
 * Suite progress tracking
 */
export interface SuiteProgress {
    /** Completed sweep parameters */
    sweepsCompleted: string[];
    /** Completed interactions */
    interactionsCompleted: string[];
    /** Current phase */
    currentPhase: string;
}
/**
 * Suite execution status
 */
export interface SuiteStatus {
    /** Current phase: 'baseline' | 'sweep' | 'interact' | 'validate' | 'report' | 'complete' | 'failed' */
    phase: string;
    /** Progress tracking */
    progress: SuiteProgress;
    /** Error message if failed */
    error?: string;
}
/**
 * Complete suite configuration
 */
export interface SuiteConfig {
    /** Unique suite ID */
    id: string;
    /** Human-readable name */
    name: string;
    /** Description */
    description?: string;
    /** Phases to run (defaults to all) */
    phases?: string[];
    /** Specific parameters to tune (defaults to all ensemble params) */
    params?: string[];
    /** Test patterns to use (defaults to representative) */
    patterns?: string[];
    /** Sweep configurations */
    sweeps?: SweepConfigItem[];
    /** Whether to resume from previous state */
    resume?: boolean;
    /** Whether to save optimal params to device after tuning */
    saveToDevice?: boolean;
    /** Save interval: 'per-pattern' | 'per-sweep' | 'per-phase' */
    saveInterval?: 'per-pattern' | 'per-sweep' | 'per-phase';
    /** Whether to analyze parameter boundaries */
    analyzeBoundaries?: boolean;
}
/**
 * Partial suite configuration for runSuite function
 */
export interface PartialSuiteConfig {
    /** Unique suite ID */
    id?: string;
    /** Human-readable name */
    name?: string;
    /** Description */
    description?: string;
    /** Phases to run (defaults to all) */
    phases?: string[];
    /** Specific parameters to tune (defaults to all ensemble params) */
    params?: string[];
    /** Test patterns to use (defaults to representative) */
    patterns?: string[];
    /** Sweep configurations */
    sweeps?: SweepConfigItem[];
    /** Whether to resume from previous state */
    resume?: boolean;
    /** Whether to save optimal params to device after tuning */
    saveToDevice?: boolean;
    /** Save interval: 'per-pattern' | 'per-sweep' | 'per-phase' */
    saveInterval?: 'per-pattern' | 'per-sweep' | 'per-phase';
    /** Whether to analyze parameter boundaries */
    analyzeBoundaries?: boolean;
}
export interface SuitePhase {
    name: string;
    description: string;
    run: (options: TunerOptions, stateManager: StateManager) => Promise<void>;
}
/**
 * Run the full calibration suite
 */
export declare function runSuite(options: TunerOptions, config?: PartialSuiteConfig): Promise<void>;
/**
 * Show summary of current state
 */
export declare function showSuiteSummary(outputDir: string): Promise<void>;
/**
 * Quick sweep of a specific parameter
 */
export declare function quickSweep(options: TunerOptions, paramName: string): Promise<void>;
/**
 * Validate current device settings
 */
export declare function validateCurrentSettings(options: TunerOptions): Promise<void>;
/**
 * Get parameter suggestions based on pattern performance
 */
export declare function getParameterSuggestions(patternId: string): ParameterDef[];
/**
 * List all available patterns
 */
export declare function listPatterns(): void;
/**
 * List all tunable parameters
 */
export declare function listParameters(): void;
/**
 * Predefined test suites for common calibration workflows
 */
export declare const PREDEFINED_SUITES: Record<string, SuiteConfig>;
/**
 * Runs a suite configuration through its phases
 */
export declare class SuiteRunner {
    private suite;
    private options;
    private stateManager;
    private status;
    constructor(suite: SuiteConfig, options: TunerOptions);
    /**
     * Run the suite
     */
    run(): Promise<SuiteStatus>;
    /**
     * Get the state manager
     */
    getStateManager(): StateManager;
    /**
     * Get current status
     */
    getStatus(): SuiteStatus;
}
/**
 * Get a predefined suite by ID
 */
export declare function getSuite(id: string): SuiteConfig | undefined;
/**
 * List all available suites
 */
export declare function listSuites(): void;
/**
 * Validate a suite configuration
 */
export declare function validateSuiteConfig(config: SuiteConfig): string[];
