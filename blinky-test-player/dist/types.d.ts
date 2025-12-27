/**
 * Types for transient detection testing system
 */
export type TransientType = 'low' | 'high';
/**
 * Ground truth annotation for test patterns
 */
export interface GroundTruthHit {
    time: number;
    type: TransientType;
    strength: number;
}
/**
 * Background audio configuration
 * Simulates continuous audio present in real music (pads, bass, synths)
 */
export interface BackgroundConfig {
    lowDrone?: {
        frequency: number;
        gain: number;
    };
    midPad?: {
        frequency: number;
        gain: number;
    };
    noiseFloor?: {
        gain: number;
    };
}
/**
 * Programmatic test pattern with built-in ground truth
 */
export interface TestPattern {
    id: string;
    name: string;
    description: string;
    durationMs: number;
    bpm?: number;
    hits: GroundTruthHit[];
    background?: BackgroundConfig;
}
/**
 * Output format for CLI
 */
export interface PatternOutput {
    pattern: string;
    durationMs: number;
    startedAt: string;
    hits: Array<{
        timeMs: number;
        type: TransientType;
        strength: number;
    }>;
}
