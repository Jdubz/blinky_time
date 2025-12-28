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
export type InstrumentType =
  | 'kick' | 'snare' | 'hat' | 'tom' | 'clap' | 'percussion' | 'bass'  // Drums + bass
  | 'synth_stab' | 'lead'    // Transient melodic (should trigger)
  | 'pad' | 'chord';          // Sustained (should NOT trigger - false positive test)

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
  synth_stab: 'high',  // Synth stabs are transient, high-frequency content
  lead: 'high',        // Lead notes with attack are transient
  pad: 'low',          // Pads are sustained, low frequency (but should NOT trigger)
  chord: 'high',       // Chord stabs can have transient attack
};

/**
 * Whether an instrument type SHOULD trigger transient detection
 * - true: Has sharp attack, should be detected (kick, snare, synth_stab, etc.)
 * - false: Sustained/slow attack, should NOT trigger (pad, chord sustain)
 */
export const INSTRUMENT_SHOULD_TRIGGER: Record<InstrumentType, boolean> = {
  kick: true,
  tom: true,
  bass: true,          // Bass notes have attack - should trigger
  snare: true,
  hat: true,           // Hats are transient but often quiet
  clap: true,
  percussion: true,
  synth_stab: true,    // Sharp synth attack - should trigger
  lead: true,          // Lead notes with attack - should trigger
  pad: false,          // Sustained pad - should NOT trigger
  chord: false,        // Sustained chord - should NOT trigger (stabs are different)
};

/**
 * Ground truth annotation for test patterns
 */
export interface GroundTruthHit {
  time: number; // Time in seconds
  type: TransientType; // Detection band (low/high)
  instrument?: InstrumentType; // Optional instrument type for sample playback
  strength: number; // 0.0 - 1.0
  expectTrigger?: boolean; // If false, this sound should NOT trigger detection (e.g., pads)
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
  startedAt: string; // ISO timestamp
  hits: Array<{
    timeMs: number;
    type: TransientType;
    instrument?: InstrumentType;
    sample?: string; // Which sample file was used
    strength: number;
  }>;
}
