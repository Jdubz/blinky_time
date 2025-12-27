/**
 * Types for transient detection testing system
 */

export type TransientType = 'low' | 'high';

/**
 * Instrument types that map to detection bands
 * - Low band (50-200 Hz): kick, tom, bass
 * - High band (2-8 kHz): snare, hat, clap, percussion
 */
export type InstrumentType = 'kick' | 'snare' | 'hat' | 'tom' | 'clap' | 'percussion' | 'bass';

/**
 * Mapping from instrument type to detection band
 */
export const INSTRUMENT_TO_BAND: Record<InstrumentType, TransientType> = {
  kick: 'low',
  tom: 'low',
  bass: 'low',
  snare: 'high',
  hat: 'high',
  clap: 'high',
  percussion: 'high',
};

/**
 * Ground truth annotation for test patterns
 */
export interface GroundTruthHit {
  time: number; // Time in seconds
  type: TransientType; // Detection band (low/high)
  instrument?: InstrumentType; // Optional instrument type for sample playback
  strength: number; // 0.0 - 1.0
}

/**
 * Programmatic test pattern with built-in ground truth
 */
export interface TestPattern {
  id: string;
  name: string;
  description: string;
  durationMs: number; // Total pattern duration
  bpm?: number; // Optional BPM for musical patterns
  hits: GroundTruthHit[]; // Automatically serves as ground truth
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
}

/**
 * Output format for CLI
 */
export interface PatternOutput {
  pattern: string;
  durationMs: number;
  startedAt: string; // ISO timestamp
  hits: Array<{
    timeMs: number;
    type: TransientType;
    instrument?: InstrumentType;
    sample?: string; // Which sample file was used
    strength: number;
  }>;
}
