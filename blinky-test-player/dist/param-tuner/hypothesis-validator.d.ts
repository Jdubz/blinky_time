/**
 * Hypothesis Validation Runner - Tests multi-hypothesis tempo tracking
 *
 * Validates:
 * - Hypothesis creation from autocorrelation peaks
 * - Promotion logic (confidence-based, min beats requirement)
 * - Tempo change tracking (gradual and abrupt)
 * - Half-time/double-time ambiguity resolution
 * - Decay behavior during silence and phrases
 */
/**
 * Single hypothesis state snapshot
 */
export interface HypothesisSnapshot {
    slot: number;
    active: boolean;
    bpm: number;
    phase: number;
    strength: number;
    confidence: number;
    beatCount: number;
    avgPhaseError: number;
    priority: number;
}
/**
 * Extended rhythm tracking state (from "json rhythm" command)
 */
export interface RhythmState {
    bpm: number;
    periodicityStrength: number;
    beatStability: number;
    tempoVelocity: number;
    nextBeatMs: number;
    tempoPriorWeight: number;
}
/**
 * Complete hypothesis tracker state at a point in time
 */
export interface HypothesisState {
    timestampMs: number;
    hypotheses: HypothesisSnapshot[];
    primaryIndex: number;
    rhythm?: RhythmState;
}
/**
 * Hypothesis validation test result
 */
export interface HypothesisValidationResult {
    pattern: string;
    durationMs: number;
    expectedBpm: number | null;
    hypotheses: {
        /** Maximum number of concurrent hypotheses */
        maxConcurrent: number;
        /** Total hypotheses created during test */
        totalCreated: number;
        /** Number of promotions to primary */
        promotions: number;
        /** Time to first hypothesis creation (ms) */
        timeToFirstMs: number | null;
    };
    primary: {
        /** Average BPM of primary hypothesis */
        avgBpm: number;
        /** BPM error vs expected (null if no expected) */
        bpmError: number | null;
        /** Average confidence of primary */
        avgConfidence: number;
        /** Confidence growth rate (conf/second) */
        confidenceGrowth: number;
        /** Average phase error */
        avgPhaseError: number;
    };
    tempoChanges?: {
        /** Number of tempo changes detected */
        changesDetected: number;
        /** Average lag to detect change (ms) */
        avgLagMs: number;
        /** Successful transitions */
        successfulTransitions: number;
    };
    ambiguity?: {
        /** Both 60 and 120 BPM hypotheses created */
        bothCreated: boolean;
        /** Correct BPM won (120 BPM) */
        correctWon: boolean;
        /** Time to resolve ambiguity (ms) */
        resolutionTimeMs: number | null;
    };
    silenceDecay?: {
        /** Hypotheses survived silence gaps */
        survived: boolean;
        /** Confidence decay rate during silence */
        decayRate: number;
        /** Grace period observed */
        gracePeriodMs: number | null;
    };
    stability?: {
        /** Average beat stability (0-1) */
        avgStability: number;
        /** Min stability observed */
        minStability: number;
        /** Max stability observed */
        maxStability: number;
        /** Stability variance */
        variance: number;
    };
    tempoPrior?: {
        /** Average tempo prior weight applied */
        avgWeight: number;
        /** Did tempo prior prevent octave error? */
        preventedOctaveError: boolean;
        /** Average tempo velocity (BPM/s) */
        avgTempoVelocity: number;
    };
    states: HypothesisState[];
}
export declare class HypothesisValidator {
    private port;
    private parser;
    private responseBuffer;
    /**
     * Connect to device
     */
    connect(portPath: string): Promise<void>;
    /**
     * Disconnect from device
     */
    disconnect(): Promise<void>;
    /**
     * Send command and get response with polling and timeout
     */
    private sendCommand;
    /**
     * Extract JSON object from response using balanced brace counting
     */
    private extractJson;
    /**
     * Get current hypothesis state
     */
    getHypothesisState(): Promise<HypothesisState>;
    /**
     * Get current rhythm state (includes beat stability, tempo prior, etc.)
     */
    getRhythmState(): Promise<RhythmState>;
    /**
     * Get combined hypothesis and rhythm state
     */
    getFullState(): Promise<HypothesisState>;
    /**
     * Run hypothesis validation test
     */
    runTest(pattern: string, expectedBpm?: number | null, gain?: number): Promise<HypothesisValidationResult>;
    /**
     * Analyze hypothesis validation results
     */
    private analyzeResults;
    /**
     * Compute variance of an array of numbers
     */
    private computeVariance;
}
/**
 * Run hypothesis validation for a single pattern
 */
export declare function validateHypothesis(portPath: string, pattern: string, expectedBpm?: number | null, gain?: number): Promise<HypothesisValidationResult>;
/**
 * Run hypothesis validation suite
 */
export declare function runHypothesisValidationSuite(portPath: string, gain?: number): Promise<HypothesisValidationResult[]>;
