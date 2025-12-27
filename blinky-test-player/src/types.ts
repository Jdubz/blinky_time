/**
 * Types for transient detection testing system
 */

export type TransientType = 'low' | 'high';

/**
 * Ground truth annotation for test patterns
 */
export interface GroundTruthHit {
  time: number; // Time in seconds
  type: TransientType;
  strength: number; // 0.0 - 1.0
}

/**
 * Background audio configuration
 * Simulates continuous audio present in real music (pads, bass, synths)
 */
export interface BackgroundConfig {
  // Low frequency drone (simulates sub-bass, kick sustain)
  lowDrone?: {
    frequency: number; // Hz (typically 40-80)
    gain: number; // 0-1
  };
  // Mid frequency pad (simulates synth pads, sustained notes)
  midPad?: {
    frequency: number; // Hz (typically 200-500)
    gain: number; // 0-1
  };
  // High frequency noise floor (simulates room tone, hi-hat bleed)
  noiseFloor?: {
    gain: number; // 0-1
  };
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
  background?: BackgroundConfig; // Continuous audio during pattern
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
    strength: number;
  }>;
}
