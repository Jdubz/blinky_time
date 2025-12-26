/**
 * Pre-defined percussion test patterns
 *
 * Each pattern defines a sequence of percussion hits with exact timing.
 * Ground truth is automatically derived from the pattern definition.
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
 * Simple 4/4 beat pattern (120 BPM, 8 bars)
 * Classic rock/pop beat: Kick on 1 and 3, snare on 2 and 4, eighth-note hi-hats
 */
export const SIMPLE_BEAT: TestPattern = {
  id: 'simple-beat',
  name: 'Simple 4/4 Beat',
  description: 'Basic rock beat - kick on 1&3, snare on 2&4, eighth hi-hats (120 BPM, 8 bars)',
  durationMs: 16000, // 8 bars at 120 BPM
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4; // 4 beats per bar

      // Kick on beats 1 and 3
      hits.push({
        time: beatToTime(barOffset + 0, bpm),
        type: 'kick',
        strength: 0.9,
      });
      hits.push({
        time: beatToTime(barOffset + 2, bpm),
        type: 'kick',
        strength: 0.9,
      });

      // Snare on beats 2 and 4
      hits.push({
        time: beatToTime(barOffset + 1, bpm),
        type: 'snare',
        strength: 0.8,
      });
      hits.push({
        time: beatToTime(barOffset + 3, bpm),
        type: 'snare',
        strength: 0.8,
      });

      // Hi-hats on eighth notes (8 per bar)
      for (let eighth = 0; eighth < 8; eighth++) {
        hits.push({
          time: beatToTime(barOffset + eighth * 0.5, bpm),
          type: 'hihat',
          strength: 0.6,
        });
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Kick drum barrage - rapid kick hits at varying intervals
 * Tests kick detection accuracy and timing precision
 */
export const KICK_BARRAGE: TestPattern = {
  id: 'kick-barrage',
  name: 'Kick Barrage',
  description: 'Rapid kick drums at varying intervals - tests kick detection accuracy',
  durationMs: 8000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const intervals = [0.5, 0.4, 0.3, 0.25, 0.2, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5]; // Seconds

    let time = 0.5; // Start at 0.5s
    for (const interval of intervals) {
      hits.push({
        time,
        type: 'kick',
        strength: 0.9,
      });
      time += interval;

      // Add a few more repeats
      for (let i = 0; i < 3; i++) {
        hits.push({
          time,
          type: 'kick',
          strength: 0.9,
        });
        time += interval;
      }
    }

    return hits.filter(h => h.time < 8.0); // Keep within 8 seconds
  })(),
};

/**
 * Snare roll - fast snare hits
 * Tests snare detection with rapid consecutive hits
 */
export const SNARE_ROLL: TestPattern = {
  id: 'snare-roll',
  name: 'Snare Roll',
  description: 'Rapid snare hits in rolls - tests snare detection accuracy',
  durationMs: 6000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];

    // Series of rolls with varying speeds
    const rolls = [
      { start: 0.5, interval: 0.2, count: 8 }, // Slow roll
      { start: 2.5, interval: 0.15, count: 10 }, // Medium roll
      { start: 4.0, interval: 0.1, count: 12 }, // Fast roll
    ];

    for (const roll of rolls) {
      for (let i = 0; i < roll.count; i++) {
        hits.push({
          time: roll.start + i * roll.interval,
          type: 'snare',
          strength: 0.8,
        });
      }
    }

    return hits;
  })(),
};

/**
 * Hi-hat groove - complex hi-hat patterns with accents
 * Tests hi-hat detection and strength variations
 */
export const HIHAT_GROOVE: TestPattern = {
  id: 'hihat-groove',
  name: 'Hi-Hat Groove',
  description: 'Complex hi-hat patterns with accents - tests classification and dynamics',
  durationMs: 8000,
  bpm: 100,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 100;
    const bars = 4;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Sixteenth-note hi-hats with accents
      for (let sixteenth = 0; sixteenth < 16; sixteenth++) {
        const isAccent = sixteenth % 4 === 0; // Accent every quarter note
        hits.push({
          time: beatToTime(barOffset + sixteenth * 0.25, bpm),
          type: 'hihat',
          strength: isAccent ? 0.9 : 0.5,
        });
      }
    }

    return hits;
  })(),
};

/**
 * Polyrhythm chaos - overlapping percussion types at different rhythms
 * Tests classification when multiple types play close together
 */
export const POLYRHYTHM_CHAOS: TestPattern = {
  id: 'polyrhythm',
  name: 'Polyrhythm Chaos',
  description:
    'Overlapping kick/snare/hihat at different rhythms - tests classification under pressure',
  durationMs: 12000,
  bpm: 90,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 90;
    const duration = 12; // 12 seconds

    // Kick in 4/4
    for (let beat = 0; beat < duration * (bpm / 60); beat += 1) {
      hits.push({
        time: beatToTime(beat, bpm),
        type: 'kick',
        strength: 0.9,
      });
    }

    // Snare in 3/4 (polyrhythm against kick)
    for (let beat = 0; beat < duration * (bpm / 60); beat += 1.5) {
      hits.push({
        time: beatToTime(beat, bpm),
        type: 'snare',
        strength: 0.8,
      });
    }

    // Hi-hat in constant eighths
    for (let beat = 0; beat < duration * (bpm / 60); beat += 0.5) {
      hits.push({
        time: beatToTime(beat, bpm),
        type: 'hihat',
        strength: 0.6,
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
  description: 'Percussion at 100ms, 150ms, 200ms, 250ms intervals - tests timing accuracy',
  durationMs: 10000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const intervals = [0.1, 0.15, 0.2, 0.25]; // Seconds

    for (const interval of intervals) {
      const sectionStart = hits.length > 0 ? hits[hits.length - 1].time + 0.5 : 0.5;

      for (let i = 0; i < 10; i++) {
        // Cycle through kick, snare, hihat
        const types: ('kick' | 'snare' | 'hihat')[] = ['kick', 'snare', 'hihat'];
        hits.push({
          time: sectionStart + i * interval,
          type: types[i % 3],
          strength: 0.8,
        });
      }
    }

    return hits.filter(h => h.time < 10.0);
  })(),
};

/**
 * All available test patterns
 */
export const TEST_PATTERNS: TestPattern[] = [
  SIMPLE_BEAT,
  KICK_BARRAGE,
  SNARE_ROLL,
  HIHAT_GROOVE,
  POLYRHYTHM_CHAOS,
  TIMING_TEST,
];
