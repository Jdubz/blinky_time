/**
 * Pre-defined transient test patterns
 *
 * Each pattern defines a sequence of transient hits with exact timing.
 * Ground truth is automatically derived from the pattern definition.
 *
 * Simplified single-band system:
 * - 'transient': Unified transient detection
 */

import type { TestPattern, GroundTruthHit } from '../types/testTypes';

/**
 * Helper to convert BPM and beat number to time in seconds
 */
function beatToTime(beat: number, bpm: number): number {
  const beatsPerSecond = bpm / 60;
  return beat / beatsPerSecond;
}

/**
 * Simple beat pattern (120 BPM, 8 bars)
 * Transients on all four beats
 */
export const SIMPLE_BEAT: TestPattern = {
  id: 'simple-beat',
  name: 'Simple Beat',
  description: 'Transients on all beats (120 BPM, 8 bars)',
  durationMs: 16000, // 8 bars at 120 BPM
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4; // 4 beats per bar

      // Transients on all beats
      hits.push({
        time: beatToTime(barOffset + 0, bpm),
        type: 'transient',
        strength: 0.9,
      });
      hits.push({
        time: beatToTime(barOffset + 1, bpm),
        type: 'transient',
        strength: 0.8,
      });
      hits.push({
        time: beatToTime(barOffset + 2, bpm),
        type: 'transient',
        strength: 0.9,
      });
      hits.push({
        time: beatToTime(barOffset + 3, bpm),
        type: 'transient',
        strength: 0.8,
      });
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Rapid barrage - rapid transients at varying intervals
 * Tests detection accuracy and timing precision
 */
export const LOW_BARRAGE: TestPattern = {
  id: 'low-barrage',
  name: 'Rapid Barrage',
  description: 'Rapid transients at varying intervals - tests detection accuracy',
  durationMs: 8000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const intervals = [0.5, 0.4, 0.3, 0.25, 0.2, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5]; // Seconds

    let time = 0.5; // Start at 0.5s
    for (const interval of intervals) {
      hits.push({
        time,
        type: 'transient',
        strength: 0.9,
      });
      time += interval;

      // Add a few more repeats
      for (let i = 0; i < 3; i++) {
        hits.push({
          time,
          type: 'transient',
          strength: 0.9,
        });
        time += interval;
      }
    }

    return hits.filter(h => h.time < 8.0); // Keep within 8 seconds
  })(),
};

/**
 * Burst pattern - rapid consecutive transients
 * Tests detection with rapid consecutive hits
 */
export const HIGH_BURST: TestPattern = {
  id: 'high-burst',
  name: 'Burst Pattern',
  description: 'Rapid consecutive transients - tests detection accuracy',
  durationMs: 6000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];

    // Series of bursts with varying speeds
    const bursts = [
      { start: 0.5, interval: 0.2, count: 8 }, // Slow burst
      { start: 2.5, interval: 0.15, count: 10 }, // Medium burst
      { start: 4.0, interval: 0.1, count: 12 }, // Fast burst
    ];

    for (const burst of bursts) {
      for (let i = 0; i < burst.count; i++) {
        hits.push({
          time: burst.start + i * burst.interval,
          type: 'transient',
          strength: 0.8,
        });
      }
    }

    return hits;
  })(),
};

/**
 * Mixed pattern - varying rhythmic intervals
 * Tests detection with complex timing
 */
export const MIXED_PATTERN: TestPattern = {
  id: 'mixed-pattern',
  name: 'Mixed Rhythm',
  description: 'Transients at varying rhythmic intervals',
  durationMs: 10000,
  bpm: 100,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 100;
    const duration = 10; // 10 seconds

    // Transients in quarter notes
    for (let beat = 0; beat < duration * (bpm / 60); beat += 1) {
      hits.push({
        time: beatToTime(beat, bpm),
        type: 'transient',
        strength: 0.9,
      });
    }

    // Additional transients in eighth notes (offset by half beat)
    for (let beat = 0.5; beat < duration * (bpm / 60); beat += 1) {
      hits.push({
        time: beatToTime(beat, bpm),
        type: 'transient',
        strength: 0.7,
      });
    }

    return hits.filter(h => h.time < duration).sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Timing precision test - hits at precise intervals
 * Tests timing accuracy with various intervals: 100ms, 150ms, 200ms, 250ms
 */
export const TIMING_TEST: TestPattern = {
  id: 'timing-test',
  name: 'Timing Precision Test',
  description: 'Transients at 100ms, 150ms, 200ms, 250ms intervals - tests timing accuracy',
  durationMs: 10000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const intervals = [0.1, 0.15, 0.2, 0.25]; // Seconds

    for (const interval of intervals) {
      const sectionStart = hits.length > 0 ? hits[hits.length - 1].time + 0.5 : 0.5;

      for (let i = 0; i < 10; i++) {
        hits.push({
          time: sectionStart + i * interval,
          type: 'transient',
          strength: 0.8,
        });
      }
    }

    return hits.filter(h => h.time < 10.0);
  })(),
};

/**
 * Sparse pattern - transients at irregular intervals
 * Tests detection with varying gaps
 */
export const SIMULTANEOUS_TEST: TestPattern = {
  id: 'simultaneous',
  name: 'Sparse Pattern',
  description: 'Transients at irregular intervals - tests detection with varying gaps',
  durationMs: 8000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const times = [0.5, 1.0, 1.5, 2.0, 2.75, 3.5, 4.5, 5.5, 6.25, 7.0];

    for (const time of times) {
      hits.push({
        time,
        type: 'transient',
        strength: 0.9,
      });
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * All available test patterns
 */
export const TEST_PATTERNS: TestPattern[] = [
  SIMPLE_BEAT,
  LOW_BARRAGE,
  HIGH_BURST,
  MIXED_PATTERN,
  TIMING_TEST,
  SIMULTANEOUS_TEST,
];
