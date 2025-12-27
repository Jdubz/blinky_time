/**
 * Pre-defined transient test patterns
 *
 * Each pattern defines a sequence of transient hits with exact timing.
 * Ground truth is automatically derived from the pattern definition.
 *
 * Two-band system:
 * - 'low': Bass transients (50-200 Hz)
 * - 'high': Brightness transients (2-8 kHz)
 */

import type { TestPattern, GroundTruthHit, TransientType } from '../types/testTypes';

/**
 * Helper to convert BPM and beat number to time in seconds
 */
function beatToTime(beat: number, bpm: number): number {
  const beatsPerSecond = bpm / 60;
  return beat / beatsPerSecond;
}

/**
 * Simple alternating pattern (120 BPM, 8 bars)
 * Low transients on 1 and 3, high transients on 2 and 4
 */
export const SIMPLE_BEAT: TestPattern = {
  id: 'simple-beat',
  name: 'Alternating Low/High',
  description: 'Low on 1&3, high on 2&4, alternating (120 BPM, 8 bars)',
  durationMs: 16000, // 8 bars at 120 BPM
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4; // 4 beats per bar

      // Low on beats 1 and 3
      hits.push({
        time: beatToTime(barOffset + 0, bpm),
        type: 'low',
        strength: 0.9,
      });
      hits.push({
        time: beatToTime(barOffset + 2, bpm),
        type: 'low',
        strength: 0.9,
      });

      // High on beats 2 and 4
      hits.push({
        time: beatToTime(barOffset + 1, bpm),
        type: 'high',
        strength: 0.8,
      });
      hits.push({
        time: beatToTime(barOffset + 3, bpm),
        type: 'high',
        strength: 0.8,
      });
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Low band barrage - rapid bass transients at varying intervals
 * Tests low-band detection accuracy and timing precision
 */
export const LOW_BARRAGE: TestPattern = {
  id: 'low-barrage',
  name: 'Low Band Barrage',
  description: 'Rapid bass transients at varying intervals - tests low-band detection accuracy',
  durationMs: 8000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const intervals = [0.5, 0.4, 0.3, 0.25, 0.2, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5]; // Seconds

    let time = 0.5; // Start at 0.5s
    for (const interval of intervals) {
      hits.push({
        time,
        type: 'low',
        strength: 0.9,
      });
      time += interval;

      // Add a few more repeats
      for (let i = 0; i < 3; i++) {
        hits.push({
          time,
          type: 'low',
          strength: 0.9,
        });
        time += interval;
      }
    }

    return hits.filter(h => h.time < 8.0); // Keep within 8 seconds
  })(),
};

/**
 * High band burst - rapid high-frequency transients
 * Tests high-band detection with rapid consecutive hits
 */
export const HIGH_BURST: TestPattern = {
  id: 'high-burst',
  name: 'High Band Burst',
  description: 'Rapid high-frequency transients - tests high-band detection accuracy',
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
          type: 'high',
          strength: 0.8,
        });
      }
    }

    return hits;
  })(),
};

/**
 * Mixed pattern - interleaved low and high with varying dynamics
 * Tests classification when both bands are active
 */
export const MIXED_PATTERN: TestPattern = {
  id: 'mixed-pattern',
  name: 'Mixed Low/High',
  description: 'Interleaved low and high transients with varying dynamics',
  durationMs: 10000,
  bpm: 100,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 100;
    const duration = 10; // 10 seconds

    // Low in quarter notes
    for (let beat = 0; beat < duration * (bpm / 60); beat += 1) {
      hits.push({
        time: beatToTime(beat, bpm),
        type: 'low',
        strength: 0.9,
      });
    }

    // High in eighth notes (offset by half beat)
    for (let beat = 0.5; beat < duration * (bpm / 60); beat += 1) {
      hits.push({
        time: beatToTime(beat, bpm),
        type: 'high',
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
        // Alternate between low and high
        const types: TransientType[] = ['low', 'high'];
        hits.push({
          time: sectionStart + i * interval,
          type: types[i % 2],
          strength: 0.8,
        });
      }
    }

    return hits.filter(h => h.time < 10.0);
  })(),
};

/**
 * Simultaneous hits - low and high at exact same time
 * Tests detection when both bands trigger simultaneously
 */
export const SIMULTANEOUS_TEST: TestPattern = {
  id: 'simultaneous',
  name: 'Simultaneous Hits',
  description: 'Low and high transients at exactly the same time - tests concurrent detection',
  durationMs: 8000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const times = [0.5, 1.0, 1.5, 2.0, 2.75, 3.5, 4.5, 5.5, 6.25, 7.0];

    for (const time of times) {
      // Both low and high at same time
      hits.push({
        time,
        type: 'low',
        strength: 0.9,
      });
      hits.push({
        time,
        type: 'high',
        strength: 0.8,
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
