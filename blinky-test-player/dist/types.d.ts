/**
 * Types for transient detection testing system
 */
export type TransientType = 'low' | 'high';
/**
 * Instrument types that map to detection bands
 * - Low band (50-200 Hz): kick, tom, bass
 * - High band (2-8 kHz): snare, hat, clap, percussion
 * - Transient: synth_stab, lead (sharp attack, should trigger)
 * - Sustained: pad, chord (slow attack, should NOT trigger)
 */
export type InstrumentType = 'kick' | 'snare' | 'hat' | 'tom' | 'clap' | 'percussion' | 'bass' | 'synth_stab' | 'lead' | 'pad' | 'chord';
/**
 * Mapping from instrument type to detection band
 */
export declare const INSTRUMENT_TO_BAND: Record<InstrumentType, TransientType>;
/**
 * Whether an instrument type SHOULD trigger transient detection
 * - true: Has sharp attack, should be detected (kick, snare, synth_stab, etc.)
 * - false: Sustained/slow attack, should NOT trigger (pad, chord sustain)
 */
export declare const INSTRUMENT_SHOULD_TRIGGER: Record<InstrumentType, boolean>;
/**
 * Ground truth annotation for test patterns
 */
export interface GroundTruthHit {
    time: number;
    type: TransientType;
    instrument?: InstrumentType;
    strength: number;
    expectTrigger?: boolean;
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
}
/**
 * Sample manifest - files available in each sample folder
 */
export interface SampleManifest {
    kick?: string[];
    snare?: string[];
    hat?: string[];
    tom?: string[];
    clap?: string[];
    percussion?: string[];
    bass?: string[];
    synth_stab?: string[];
    lead?: string[];
    pad?: string[];
    chord?: string[];
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
        instrument?: InstrumentType;
        sample?: string;
        strength: number;
    }>;
}
