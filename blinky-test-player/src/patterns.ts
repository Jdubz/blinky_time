/**
 * Pre-defined transient test patterns using real samples
 *
 * Each pattern defines a sequence of instrument hits with exact timing.
 * Ground truth is automatically derived from instrument → band mapping.
 *
 * Instruments and their detection bands:
 * - Low band (50-200 Hz): kick, tom, bass
 * - High band (2-8 kHz): snare, hat, clap, percussion
 */

import type { TestPattern, GroundTruthHit, InstrumentType, ExtendedTestPattern, PatternRegistry, PatternMetadata, PatternCategory } from './types.js';
import { INSTRUMENT_TO_BAND, INSTRUMENT_SHOULD_TRIGGER } from './types.js';

/**
 * Helper to convert BPM and beat number to time in seconds
 */
function beatToTime(beat: number, bpm: number): number {
  const beatsPerSecond = bpm / 60;
  return beat / beatsPerSecond;
}

/**
 * Helper to create a hit with automatic band detection and trigger expectation
 */
function hit(time: number, instrument: InstrumentType, strength: number = 0.9): GroundTruthHit {
  return {
    time,
    type: INSTRUMENT_TO_BAND[instrument],
    instrument,
    strength,
    expectTrigger: INSTRUMENT_SHOULD_TRIGGER[instrument],
  };
}

/**
 * Basic drum pattern (120 BPM, 8 bars)
 * Kick on 1 and 3, snare on 2 and 4, hats on 8th notes
 */
export const BASIC_DRUMS: TestPattern = {
  id: 'basic-drums',
  name: 'Basic Drum Pattern',
  description: 'Kick on 1&3, snare on 2&4, hats on 8th notes (120 BPM, 8 bars)',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4; // 4 beats per bar

      // Kick on beats 1 and 3
      hits.push(hit(beatToTime(barOffset + 0, bpm), 'kick'));
      hits.push(hit(beatToTime(barOffset + 2, bpm), 'kick'));

      // Snare on beats 2 and 4
      hits.push(hit(beatToTime(barOffset + 1, bpm), 'snare'));
      hits.push(hit(beatToTime(barOffset + 3, bpm), 'snare'));

      // Hats on 8th notes
      for (let eighth = 0; eighth < 8; eighth++) {
        hits.push(hit(beatToTime(barOffset + eighth * 0.5, bpm), 'hat', 0.6));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Kick focus pattern - various kick patterns
 * Tests low-band detection accuracy
 */
export const KICK_FOCUS: TestPattern = {
  id: 'kick-focus',
  name: 'Kick Focus',
  description: 'Various kick patterns at different intervals - tests low-band detection',
  durationMs: 12000,
  bpm: 100,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 100;

    // Section 1: Quarter notes (0-4s)
    for (let beat = 0; beat < 8; beat++) {
      hits.push(hit(beatToTime(beat, bpm), 'kick'));
    }

    // Section 2: 8th notes (5-8s) - faster kicks
    for (let beat = 10; beat < 16; beat += 0.5) {
      hits.push(hit(beatToTime(beat, bpm), 'kick', 0.85));
    }

    // Section 3: Syncopated (9-12s)
    const syncopated = [0, 0.75, 1.5, 2, 2.5, 3.25, 3.75];
    for (const b of syncopated) {
      hits.push(hit(beatToTime(18 + b, bpm), 'kick'));
    }

    return hits.filter(h => h.time < 12);
  })(),
};

/**
 * Snare focus pattern
 * Tests high-band detection with snare variations
 */
export const SNARE_FOCUS: TestPattern = {
  id: 'snare-focus',
  name: 'Snare Focus',
  description: 'Various snare patterns including rolls - tests high-band detection',
  durationMs: 10000,
  bpm: 110,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 110;

    // Section 1: Backbeat (0-4s)
    for (let bar = 0; bar < 4; bar++) {
      const barOffset = bar * 4;
      hits.push(hit(beatToTime(barOffset + 1, bpm), 'snare'));
      hits.push(hit(beatToTime(barOffset + 3, bpm), 'snare'));
    }

    // Section 2: Snare roll (5-7s)
    for (let beat = 10; beat < 14; beat += 0.25) {
      hits.push(hit(beatToTime(beat, bpm), 'snare', 0.7));
    }

    // Section 3: Ghost notes + accents (7-10s)
    for (let bar = 0; bar < 2; bar++) {
      const barOffset = 14 + bar * 4;
      hits.push(hit(beatToTime(barOffset + 1, bpm), 'snare', 1.0)); // Accent
      hits.push(hit(beatToTime(barOffset + 1.5, bpm), 'snare', 0.4)); // Ghost
      hits.push(hit(beatToTime(barOffset + 2.75, bpm), 'snare', 0.4)); // Ghost
      hits.push(hit(beatToTime(barOffset + 3, bpm), 'snare', 1.0)); // Accent
    }

    return hits.filter(h => h.time < 10);
  })(),
};

/**
 * Hi-hat patterns
 * Tests high-band detection with hat variations
 */
export const HAT_PATTERNS: TestPattern = {
  id: 'hat-patterns',
  name: 'Hi-Hat Patterns',
  description: 'Various hi-hat patterns: 8ths, 16ths, offbeats',
  durationMs: 12000,
  bpm: 125,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 125;

    // Section 1: 8th notes (0-4s)
    for (let beat = 0; beat < 8; beat += 0.5) {
      hits.push(hit(beatToTime(beat, bpm), 'hat', 0.7));
    }

    // Section 2: 16th notes (4-8s)
    for (let beat = 8; beat < 16; beat += 0.25) {
      const accent = beat % 1 === 0 ? 0.8 : 0.5;
      hits.push(hit(beatToTime(beat, bpm), 'hat', accent));
    }

    // Section 3: Offbeat only (8-12s)
    for (let beat = 16; beat < 24; beat += 1) {
      hits.push(hit(beatToTime(beat + 0.5, bpm), 'hat', 0.75));
    }

    return hits.filter(h => h.time < 12);
  })(),
};

/**
 * Full kit pattern - kick, snare, hat, tom, clap
 * Tests all instrument types together
 */
export const FULL_KIT: TestPattern = {
  id: 'full-kit',
  name: 'Full Drum Kit',
  description: 'All drum elements: kick, snare, hat, tom, clap',
  durationMs: 16000,
  bpm: 115,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 115;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Kick pattern
      hits.push(hit(beatToTime(barOffset + 0, bpm), 'kick'));
      hits.push(hit(beatToTime(barOffset + 2.5, bpm), 'kick', 0.8));

      // Snare on 2 and 4
      hits.push(hit(beatToTime(barOffset + 1, bpm), 'snare'));
      hits.push(hit(beatToTime(barOffset + 3, bpm), 'snare'));

      // Hi-hats on 8ths
      for (let eighth = 0; eighth < 8; eighth++) {
        hits.push(hit(beatToTime(barOffset + eighth * 0.5, bpm), 'hat', 0.5));
      }

      // Tom fill every 4 bars
      if (bar % 4 === 3) {
        hits.push(hit(beatToTime(barOffset + 3.5, bpm), 'tom', 0.9));
        hits.push(hit(beatToTime(barOffset + 3.75, bpm), 'tom', 0.85));
      }

      // Clap layered with snare on beat 3 every 2 bars
      if (bar % 2 === 1) {
        hits.push(hit(beatToTime(barOffset + 3, bpm), 'clap', 0.7));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Simultaneous hits - kick + snare, kick + clap
 * Tests detection when multiple bands trigger at once
 */
export const SIMULTANEOUS_HITS: TestPattern = {
  id: 'simultaneous',
  name: 'Simultaneous Hits',
  description: 'Kick + snare/clap at same time - tests concurrent detection',
  durationMs: 10000,
  bpm: 100,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 100;

    for (let beat = 0; beat < 16; beat += 2) {
      const time = beatToTime(beat, bpm);
      // Kick + snare together
      hits.push(hit(time, 'kick'));
      hits.push(hit(time, 'snare'));

      // Kick + clap together (offset by 1 beat)
      const time2 = beatToTime(beat + 1, bpm);
      hits.push(hit(time2, 'kick', 0.8));
      hits.push(hit(time2, 'clap', 0.8));
    }

    return hits.filter(h => h.time < 10).sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Fast tempo test (150 BPM)
 * Tests detection at higher speeds
 */
export const FAST_TEMPO: TestPattern = {
  id: 'fast-tempo',
  name: 'Fast Tempo (150 BPM)',
  description: 'High-speed drum pattern - tests detection at fast tempos',
  durationMs: 10000,
  bpm: 150,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 150;

    for (let bar = 0; bar < 6; bar++) {
      const barOffset = bar * 4;

      // Kick on every beat
      for (let beat = 0; beat < 4; beat++) {
        hits.push(hit(beatToTime(barOffset + beat, bpm), 'kick', 0.85));
      }

      // Snare on 2 and 4
      hits.push(hit(beatToTime(barOffset + 1, bpm), 'snare'));
      hits.push(hit(beatToTime(barOffset + 3, bpm), 'snare'));

      // 16th note hats
      for (let sixteenth = 0; sixteenth < 16; sixteenth++) {
        hits.push(hit(beatToTime(barOffset + sixteenth * 0.25, bpm), 'hat', 0.4));
      }
    }

    return hits.filter(h => h.time < 10).sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Sparse pattern - widely spaced hits
 * Tests detection with long gaps between transients
 */
export const SPARSE_PATTERN: TestPattern = {
  id: 'sparse',
  name: 'Sparse Pattern',
  description: 'Widely spaced hits - tests detection after silence periods',
  durationMs: 15000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];

    // Hits at irregular, wide intervals
    const times = [0.5, 2.0, 3.5, 6.0, 8.0, 9.5, 12.0, 14.0];
    const instruments: InstrumentType[] = ['kick', 'snare', 'kick', 'tom', 'kick', 'clap', 'snare', 'kick'];

    for (let i = 0; i < times.length; i++) {
      hits.push(hit(times[i], instruments[i]));
    }

    return hits;
  })(),
};

// ============================================================================
// CALIBRATED PATTERNS - Deterministic samples with known loudness
// These patterns use specific sample IDs from the curated manifest for
// reproducible testing and precise characterization of detection performance.
// ============================================================================

/**
 * Helper to create a deterministic hit with a specific sample ID
 */
function deterministicHit(
  time: number,
  sampleId: string,
  strength: number = 1.0
): GroundTruthHit {
  // Extract type from sampleId (e.g., "kick_hard_1" -> "kick", "synth_stab_hard_1" -> "synth_stab")
  // Format: <type>_<loudness>_<variant> where type can be compound (e.g., "synth_stab")
  const parts = sampleId.split('_');
  const loudnessLevels = ['hard', 'medium', 'soft', 'slow'];

  // Find the loudness marker to determine where the type ends
  let loudnessIndex = parts.findIndex(p => loudnessLevels.includes(p));
  if (loudnessIndex === -1) {
    throw new Error(`Invalid sampleId format: "${sampleId}" - missing loudness level (hard/medium/soft/slow)`);
  }

  // Everything before the loudness marker is the instrument type
  const type = parts.slice(0, loudnessIndex).join('_');
  const instrument = type as InstrumentType;

  // Validate the instrument type exists in our mappings
  if (!(instrument in INSTRUMENT_TO_BAND)) {
    throw new Error(`Unknown instrument type: "${type}" from sampleId "${sampleId}"`);
  }

  return {
    time,
    type: INSTRUMENT_TO_BAND[instrument],
    instrument,
    strength,
    sampleId, // New field for deterministic selection
    expectTrigger: INSTRUMENT_SHOULD_TRIGGER[instrument],
  } as GroundTruthHit & { sampleId: string };
}

/**
 * CALIBRATED: Strong beats only (120 BPM, 8 bars)
 * Uses only hard kick and snare samples - should be easy to detect
 * Expected: ~100% recall with any reasonable threshold
 */
export const STRONG_BEATS: TestPattern = {
  id: 'strong-beats',
  name: 'Strong Beats (Calibrated)',
  description: 'Hard kicks and snares only - baseline detection test',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      // Hard kicks on 1 and 3, alternating samples
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 1.0));
      // Hard snares on 2 and 4, alternating samples
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Medium beats (120 BPM, 8 bars)
 * Uses medium kick and snare samples - moderate detection challenge
 */
export const MEDIUM_BEATS: TestPattern = {
  id: 'medium-beats',
  name: 'Medium Beats (Calibrated)',
  description: 'Medium loudness kicks and snares - moderate detection challenge',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_medium_1', 0.7));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_medium_2', 0.7));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_medium_1', 0.7));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_medium_2', 0.7));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Soft beats (120 BPM, 8 bars)
 * Uses soft kick and snare samples - difficult detection, tests sensitivity
 */
export const SOFT_BEATS: TestPattern = {
  id: 'soft-beats',
  name: 'Soft Beats (Calibrated)',
  description: 'Soft kicks and snares - tests detection sensitivity limits',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_soft_1', 0.4));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_soft_2', 0.4));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_soft_1', 0.4));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_soft_2', 0.4));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Hat rejection test (120 BPM, 8 bars)
 * Hard kicks/snares with soft hi-hats - tests ability to ignore weak transients
 * Expected: Detect kicks/snares, reject hi-hats
 */
export const HAT_REJECTION: TestPattern = {
  id: 'hat-rejection',
  name: 'Hat Rejection (Calibrated)',
  description: 'Hard kicks/snares + soft hats - tests hi-hat rejection',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Hard kicks on 1 and 3 (should detect)
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 1.0));

      // Hard snares on 2 and 4 (should detect)
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));

      // Soft hi-hats on 8th notes (should NOT detect)
      for (let eighth = 0; eighth < 8; eighth++) {
        hits.push(deterministicHit(beatToTime(barOffset + eighth * 0.5, bpm), 'hat_soft_1', 0.3));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Mixed dynamics (120 BPM, 8 bars)
 * Realistic pattern with varying loudness - simulates real music
 */
export const MIXED_DYNAMICS: TestPattern = {
  id: 'mixed-dynamics',
  name: 'Mixed Dynamics (Calibrated)',
  description: 'Varying loudness pattern - realistic music simulation',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      const isEvenBar = bar % 2 === 0;

      // Kicks: hard on downbeats, medium on offbeats
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), isEvenBar ? 'kick_hard_1' : 'kick_medium_1', isEvenBar ? 1.0 : 0.7));

      // Snares: alternating hard/medium
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), isEvenBar ? 'snare_hard_1' : 'snare_medium_1', isEvenBar ? 1.0 : 0.7));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));

      // Hats: medium on downbeats, soft on upbeats
      for (let eighth = 0; eighth < 8; eighth++) {
        const isDownbeat = eighth % 2 === 0;
        const hatSample = isDownbeat ? 'hat_medium_1' : 'hat_soft_1';
        const hatStrength = isDownbeat ? 0.5 : 0.3;
        hits.push(deterministicHit(beatToTime(barOffset + eighth * 0.5, bpm), hatSample, hatStrength));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Tempo sweep (4 bars each at 80, 100, 120, 140 BPM)
 * Tests detection across tempo range
 */
export const TEMPO_SWEEP: TestPattern = {
  id: 'tempo-sweep',
  name: 'Tempo Sweep (Calibrated)',
  description: 'Tests detection at 80, 100, 120, 140 BPM',
  durationMs: 16000,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const tempos = [80, 100, 120, 140];
    let currentTime = 0;

    for (const bpm of tempos) {
      const beatDuration = 60 / bpm;
      const sectionDuration = 4 * beatDuration; // 4 beats per section

      for (let beat = 0; beat < 4; beat++) {
        const time = currentTime + beat * beatDuration;
        // Kick on 1 and 3
        if (beat === 0 || beat === 2) {
          hits.push(deterministicHit(time, 'kick_hard_2', 1.0));
        }
        // Snare on 2 and 4
        if (beat === 1 || beat === 3) {
          hits.push(deterministicHit(time, 'snare_hard_1', 1.0));
        }
      }

      currentTime += sectionDuration;
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

// ============================================================================
// MELODIC/HARMONIC PATTERNS - Tests with bass, synth, lead, pad, chord
// These patterns test detection accuracy with non-drum content
// ============================================================================

/**
 * CALIBRATED: Bass line with kicks (120 BPM, 8 bars)
 * Tests bass note detection alongside kicks
 * Expected: Both kicks AND bass notes should trigger (both are transients)
 */
export const BASS_LINE: TestPattern = {
  id: 'bass-line',
  name: 'Bass Line (Calibrated)',
  description: 'Kicks + bass notes - tests low frequency transient detection',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Kick on beat 1
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));

      // Bass notes on offbeats (should also trigger!)
      hits.push(deterministicHit(beatToTime(barOffset + 0.5, bpm), 'bass_hard_1', 0.8));
      hits.push(deterministicHit(beatToTime(barOffset + 1.5, bpm), 'bass_medium_1', 0.6));
      hits.push(deterministicHit(beatToTime(barOffset + 2.5, bpm), 'bass_hard_2', 0.8));
      hits.push(deterministicHit(beatToTime(barOffset + 3.5, bpm), 'bass_medium_2', 0.6));

      // Snare on 2 and 4
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Synth stab pattern (120 BPM, 8 bars)
 * Sharp synth stabs that SHOULD trigger transient detection
 */
export const SYNTH_STABS: TestPattern = {
  id: 'synth-stabs',
  name: 'Synth Stabs (Calibrated)',
  description: 'Sharp synth stabs - should trigger transient detection',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Kick on 1
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));

      // Synth stabs on offbeats
      hits.push(deterministicHit(beatToTime(barOffset + 0.5, bpm), 'synth_stab_hard_1', 0.85));
      hits.push(deterministicHit(beatToTime(barOffset + 2.5, bpm), 'synth_stab_hard_2', 0.8));

      // Snare on 2 and 4
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Lead melody with drum accents (100 BPM, 8 bars)
 * Tests if lead note attacks are detected alongside drums
 */
export const LEAD_MELODY: TestPattern = {
  id: 'lead-melody',
  name: 'Lead Melody (Calibrated)',
  description: 'Lead notes + drums - tests melodic transient detection',
  durationMs: 19200, // 8 bars at 100 BPM
  bpm: 100,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 100;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Kick on 1 and 3
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_medium_1', 0.7));

      // Lead melody notes (arpeggiated pattern)
      hits.push(deterministicHit(beatToTime(barOffset + 0.5, bpm), 'lead_hard_1', 0.75));
      hits.push(deterministicHit(beatToTime(barOffset + 1.0, bpm), 'lead_medium_1', 0.5));
      hits.push(deterministicHit(beatToTime(barOffset + 1.5, bpm), 'lead_hard_2', 0.7));
      hits.push(deterministicHit(beatToTime(barOffset + 2.5, bpm), 'lead_medium_1', 0.5));
      hits.push(deterministicHit(beatToTime(barOffset + 3.0, bpm), 'lead_hard_1', 0.75));
      hits.push(deterministicHit(beatToTime(barOffset + 3.5, bpm), 'lead_soft_1', 0.3));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Pad rejection test (80 BPM, 8 bars)
 * Sustained pads playing throughout with sparse drum hits
 * Pads should NOT trigger - only drums should be detected
 * Tests false positive rejection for sustained sounds
 */
export const PAD_REJECTION: TestPattern = {
  id: 'pad-rejection',
  name: 'Pad Rejection (Calibrated)',
  description: 'Sustained pads + sparse drums - pads should NOT trigger',
  durationMs: 24000, // 8 bars at 80 BPM
  bpm: 80,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 80;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Sparse drum hits (should be detected - expectTrigger: true)
      if (bar % 2 === 0) {
        hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
        hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'snare_hard_1', 1.0));
      }

      // Pads playing on every bar - these should NOT trigger!
      // They play but expectTrigger will be false (from INSTRUMENT_SHOULD_TRIGGER)
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'pad_slow_1', 0.7));
      if (bar % 2 === 1) {
        hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'pad_slow_2', 0.65));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Chord rejection test (90 BPM, 8 bars)
 * Sustained chords playing with drum hits
 * Chords should NOT trigger - only drums should be detected
 */
export const CHORD_REJECTION: TestPattern = {
  id: 'chord-rejection',
  name: 'Chord Rejection (Calibrated)',
  description: 'Sustained chords + drums - chords should NOT trigger',
  durationMs: 21333, // 8 bars at 90 BPM
  bpm: 90,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 90;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Drum hits (should be detected - expectTrigger: true)
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'snare_hard_1', 1.0));

      // Chords playing - these should NOT trigger!
      // They play but expectTrigger will be false (from INSTRUMENT_SHOULD_TRIGGER)
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'chord_slow_1', 0.6));
      if (bar % 2 === 0) {
        hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'chord_slow_2', 0.55));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * CALIBRATED: Full mix (120 BPM, 8 bars)
 * Realistic EDM-style mix with drums, bass, synths, leads
 * Tests detection in complex, layered audio
 */
export const FULL_MIX: TestPattern = {
  id: 'full-mix',
  name: 'Full Mix (Calibrated)',
  description: 'Drums + bass + synth + lead - realistic music simulation',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // === DRUMS ===
      // Kick on 1 and 3
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 0.9));

      // Snare on 2 and 4
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));

      // Hi-hats (soft - may or may not trigger depending on threshold)
      for (let eighth = 0; eighth < 8; eighth++) {
        hits.push(deterministicHit(beatToTime(barOffset + eighth * 0.5, bpm), 'hat_soft_1', 0.3));
      }

      // === BASS ===
      // Bass on offbeats
      hits.push(deterministicHit(beatToTime(barOffset + 0.5, bpm), 'bass_hard_1', 0.75));
      hits.push(deterministicHit(beatToTime(barOffset + 1.5, bpm), 'bass_medium_1', 0.55));
      hits.push(deterministicHit(beatToTime(barOffset + 2.5, bpm), 'bass_hard_2', 0.75));
      hits.push(deterministicHit(beatToTime(barOffset + 3.5, bpm), 'bass_medium_2', 0.55));

      // === SYNTH STABS (every other bar) ===
      if (bar % 2 === 0) {
        hits.push(deterministicHit(beatToTime(barOffset + 1.5, bpm), 'synth_stab_hard_1', 0.7));
        hits.push(deterministicHit(beatToTime(barOffset + 3.5, bpm), 'synth_stab_medium_1', 0.5));
      }

      // === LEAD (every 4th bar - melody fragment) ===
      if (bar % 4 === 2) {
        hits.push(deterministicHit(beatToTime(barOffset + 0.75, bpm), 'lead_hard_1', 0.65));
        hits.push(deterministicHit(beatToTime(barOffset + 1.25, bpm), 'lead_medium_1', 0.45));
        hits.push(deterministicHit(beatToTime(barOffset + 2.75, bpm), 'lead_hard_2', 0.65));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

// =============================================================================
// PARAMETER-TARGETED PATTERNS - Designed to test specific parameters
// =============================================================================

/**
 * Cooldown stress test - hits every 20ms
 * Tests minimum cooldown capability
 */
export const COOLDOWN_STRESS_20MS: TestPattern = {
  id: 'cooldown-stress-20ms',
  name: 'Cooldown Stress 20ms',
  description: 'Rapid hits every 20ms to stress-test minimum cooldown',
  durationMs: 4000,
  bpm: undefined,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    // 20ms intervals for 4 seconds = 200 hits
    for (let t = 0; t < 4; t += 0.02) {
      hits.push(hit(t, 'kick', 0.9));
    }
    return hits;
  })(),
};

/**
 * Cooldown stress test - hits every 40ms
 * Tests moderate cooldown
 */
export const COOLDOWN_STRESS_40MS: TestPattern = {
  id: 'cooldown-stress-40ms',
  name: 'Cooldown Stress 40ms',
  description: 'Rapid hits every 40ms to test moderate cooldown',
  durationMs: 4000,
  bpm: undefined,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    for (let t = 0; t < 4; t += 0.04) {
      hits.push(hit(t, 'kick', 0.9));
    }
    return hits;
  })(),
};

/**
 * Cooldown stress test - hits every 80ms
 * Tests standard cooldown
 */
export const COOLDOWN_STRESS_80MS: TestPattern = {
  id: 'cooldown-stress-80ms',
  name: 'Cooldown Stress 80ms',
  description: 'Hits every 80ms to test standard cooldown',
  durationMs: 4000,
  bpm: undefined,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    for (let t = 0; t < 4; t += 0.08) {
      hits.push(hit(t, 'kick', 0.9));
    }
    return hits;
  })(),
};

/**
 * Threshold gradient - hits with decreasing strength
 * Tests hitthresh and fluxthresh sensitivity
 */
export const THRESHOLD_GRADIENT: TestPattern = {
  id: 'threshold-gradient',
  name: 'Threshold Gradient',
  description: 'Hits with decreasing strength to test threshold sensitivity',
  durationMs: 10000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    // Start loud, gradually get quieter
    const strengths = [1.0, 0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.25, 0.2, 0.15, 0.1];
    strengths.forEach((strength, i) => {
      hits.push(hit(beatToTime(i * 2, bpm), 'kick', strength));
      hits.push(hit(beatToTime(i * 2 + 1, bpm), 'snare', strength));
    });
    return hits;
  })(),
};

/**
 * Sharp attack patterns - tests attackmult sensitivity to fast transients
 */
export const ATTACK_SHARP: TestPattern = {
  id: 'attack-sharp',
  name: 'Sharp Attack',
  description: 'Very fast attack transients (sharp hits)',
  durationMs: 8000,
  bpm: 100,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 100;
    // Strong, sharp attacks
    for (let beat = 0; beat < 16; beat++) {
      // Alternate between kick and snare for variety
      hits.push(hit(beatToTime(beat, bpm), beat % 2 === 0 ? 'kick' : 'snare', 0.95));
    }
    return hits;
  })(),
};

/**
 * Gradual attack patterns - slower envelope swells (should NOT trigger)
 */
export const ATTACK_GRADUAL: TestPattern = {
  id: 'attack-gradual',
  name: 'Gradual Attack',
  description: 'Slow envelope swells - should NOT trigger detection',
  durationMs: 8000,
  bpm: 60,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 60;
    // Slow pads that should NOT trigger
    for (let beat = 0; beat < 8; beat++) {
      hits.push({
        time: beatToTime(beat * 2, bpm),
        type: 'low',
        instrument: 'pad',
        strength: 0.7,
        expectTrigger: false,
      });
    }
    return hits;
  })(),
};

/**
 * Low frequency only - tests bass detection
 */
export const FREQ_LOW_ONLY: TestPattern = {
  id: 'freq-low-only',
  name: 'Low Frequency Only',
  description: 'Sub-100Hz content only for bass detection testing',
  durationMs: 8000,
  bpm: 90,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 90;
    for (let beat = 0; beat < 12; beat++) {
      hits.push(hit(beatToTime(beat, bpm), 'kick', 0.85));
      if (beat % 2 === 0) {
        hits.push(hit(beatToTime(beat + 0.5, bpm), 'bass', 0.6));
      }
    }
    return hits;
  })(),
};

/**
 * High frequency only - tests HFC detection
 */
export const FREQ_HIGH_ONLY: TestPattern = {
  id: 'freq-high-only',
  name: 'High Frequency Only',
  description: 'High frequency content (hats, cymbals) for HFC testing',
  durationMs: 8000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    for (let beat = 0; beat < 16; beat++) {
      hits.push(hit(beatToTime(beat, bpm), 'hat', 0.6));
      if (beat % 2 === 1) {
        hits.push(hit(beatToTime(beat, bpm), 'clap', 0.8));
      }
    }
    return hits;
  })(),
};

// =============================================================================
// MUSIC MODE PATTERNS - Designed to test BPM/rhythm tracking
// =============================================================================

/**
 * Steady 120 BPM - baseline for BPM tracking
 * Alternates between sample variants for realistic phrase structure
 */
export const STEADY_120BPM: TestPattern = {
  id: 'steady-120bpm',
  name: 'Steady 120 BPM',
  description: 'Consistent 120 BPM with alternating samples for reliable BPM tracking',
  durationMs: 30000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 15;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      // Alternate sample variants like strong-beats pattern
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Steady 80 BPM - slow tempo tracking
 * Alternates between sample variants for realistic phrase structure
 */
export const STEADY_80BPM: TestPattern = {
  id: 'steady-80bpm',
  name: 'Steady 80 BPM',
  description: 'Slow 80 BPM with alternating samples for reliable tracking',
  durationMs: 30000,
  bpm: 80,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 80;
    const bars = 10;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Steady 160 BPM - fast tempo tracking
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export const STEADY_160BPM: TestPattern = {
  id: 'steady-160bpm',
  name: 'Steady 160 BPM',
  description: 'Fast 160 BPM with consistent samples for reliable tracking',
  durationMs: 30000,
  bpm: 160,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 160;
    const bars = 20; // 20 bars at 160 BPM = 30 seconds

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_2', 0.95));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_1', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Tempo ramp - gradual BPM change from 80 to 160
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export const TEMPO_RAMP: TestPattern = {
  id: 'tempo-ramp',
  name: 'Tempo Ramp 80→160',
  description: 'Gradual tempo increase from 80 to 160 BPM with consistent samples',
  durationMs: 30000,
  bpm: undefined, // Variable BPM
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    let currentTime = 0;
    const totalDuration = 30;
    let hitCount = 0;

    while (currentTime < totalDuration) {
      const progress = currentTime / totalDuration;
      const currentBpm = 80 + progress * 80;
      const beatInterval = 60 / currentBpm;

      // Use consistent samples (kick on even beats, snare on odd)
      const sampleId = hitCount % 2 === 0 ? 'kick_hard_2' : 'snare_hard_1';
      hits.push(deterministicHit(currentTime, sampleId, 0.95));
      currentTime += beatInterval;
      hitCount++;
    }

    return hits;
  })(),
};

/**
 * Tempo sudden - abrupt BPM changes
 */
export const TEMPO_SUDDEN: TestPattern = {
  id: 'tempo-sudden',
  name: 'Tempo Sudden Changes',
  description: 'Abrupt BPM changes to test tempo lock recovery',
  durationMs: 24000,
  bpm: undefined,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    // 4 sections of 6 seconds each with different BPMs
    const sections = [
      { bpm: 120, startTime: 0, duration: 6 },
      { bpm: 90, startTime: 6, duration: 6 },
      { bpm: 140, startTime: 12, duration: 6 },
      { bpm: 100, startTime: 18, duration: 6 },
    ];

    for (const section of sections) {
      const beatsPerSecond = section.bpm / 60;
      const totalBeats = Math.floor(section.duration * beatsPerSecond);
      for (let beat = 0; beat < totalBeats; beat++) {
        const time = section.startTime + beat / beatsPerSecond;
        hits.push(hit(time, beat % 2 === 0 ? 'kick' : 'snare', 0.85));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Phase on-beat - hits exactly on expected beats
 */
export const PHASE_ON_BEAT: TestPattern = {
  id: 'phase-on-beat',
  name: 'Phase On-Beat',
  description: 'Hits exactly on beat for phase alignment testing',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    // Perfect quarter note pattern
    for (let beat = 0; beat < 32; beat++) {
      hits.push(hit(beatToTime(beat, bpm), 'kick', 0.9));
    }
    return hits;
  })(),
};

/**
 * Phase off-beat - syncopated hits
 */
export const PHASE_OFF_BEAT: TestPattern = {
  id: 'phase-off-beat',
  name: 'Phase Off-Beat',
  description: 'Syncopated hits to test phase tracking with off-beats',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    // Off-beat pattern (8th note offsets)
    for (let beat = 0; beat < 32; beat++) {
      // Hit on the "and" (8th note after beat)
      hits.push(hit(beatToTime(beat + 0.5, bpm), 'snare', 0.85));
    }
    return hits;
  })(),
};

/**
 * Non-musical random - random timing for music mode rejection
 */
export const NON_MUSICAL_RANDOM: TestPattern = {
  id: 'non-musical-random',
  name: 'Non-Musical Random',
  description: 'Random hit timing - should NOT trigger music mode',
  durationMs: 20000,
  bpm: undefined,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    // Use a seeded pseudo-random sequence for reproducibility
    let seed = 42;
    const random = () => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return seed / 0x7fffffff;
    };

    // Generate ~50 random hits over 20 seconds
    for (let i = 0; i < 50; i++) {
      const time = random() * 19.5 + 0.25; // 0.25 to 19.75 seconds
      hits.push(hit(time, random() > 0.5 ? 'kick' : 'snare', 0.7 + random() * 0.25));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Non-musical clustered - irregular clusters
 */
export const NON_MUSICAL_CLUSTERED: TestPattern = {
  id: 'non-musical-clustered',
  name: 'Non-Musical Clustered',
  description: 'Irregular clusters of hits - should confuse BPM tracking',
  durationMs: 20000,
  bpm: undefined,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    // Clusters at irregular intervals
    const clusterStarts = [0.5, 3.2, 5.1, 9.7, 12.3, 15.8, 18.2];
    const clusterSizes = [3, 5, 2, 7, 4, 3, 6];

    clusterStarts.forEach((start, idx) => {
      const size = clusterSizes[idx];
      for (let i = 0; i < size; i++) {
        // Hits within cluster are close together (30-80ms apart)
        const offset = i * (0.03 + 0.05 * (i % 2));
        hits.push(hit(start + offset, i % 2 === 0 ? 'kick' : 'snare', 0.8));
      }
    });

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Silence gaps - tests music mode deactivation
 */
export const SILENCE_GAPS: TestPattern = {
  id: 'silence-gaps',
  name: 'Silence Gaps',
  description: 'Music with 5-10s silence gaps to test deactivation',
  durationMs: 45000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;

    // Section 1: 0-8s at 120 BPM
    for (let beat = 0; beat < 16; beat++) {
      hits.push(hit(beatToTime(beat, bpm), beat % 2 === 0 ? 'kick' : 'snare', 0.85));
    }

    // Gap: 8-15s (7 seconds of silence)

    // Section 2: 15-23s at 120 BPM
    for (let beat = 0; beat < 16; beat++) {
      hits.push(hit(15 + beatToTime(beat, bpm), beat % 2 === 0 ? 'kick' : 'snare', 0.85));
    }

    // Gap: 23-33s (10 seconds of silence)

    // Section 3: 33-45s at 120 BPM
    for (let beat = 0; beat < 24; beat++) {
      hits.push(hit(33 + beatToTime(beat, bpm), beat % 2 === 0 ? 'kick' : 'snare', 0.85));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Steady 60 BPM - tests tempo prior's subharmonic rejection
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export const STEADY_60BPM: TestPattern = {
  id: 'steady-60bpm',
  name: 'Steady 60 BPM',
  description: 'Very slow tempo with consistent samples - tests double-time rejection',
  durationMs: 32000,
  bpm: 60,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 60;
    const bars = 8; // 8 bars at 60 BPM = 32 seconds

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_2', 0.95));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_1', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Steady 90 BPM - intermediate tempo
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export const STEADY_90BPM: TestPattern = {
  id: 'steady-90bpm',
  name: 'Steady 90 BPM',
  description: 'Intermediate tempo with consistent samples',
  durationMs: 26667,
  bpm: 90,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 90;
    const bars = 10; // 10 bars at 90 BPM ≈ 27 seconds

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_2', 0.95));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_1', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Steady 180 BPM - fast tempo subharmonic test
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export const STEADY_180BPM: TestPattern = {
  id: 'steady-180bpm',
  name: 'Steady 180 BPM',
  description: 'Very fast tempo with consistent samples - tests half-time rejection',
  durationMs: 26667,
  bpm: 180,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 180;
    const bars = 20; // 20 bars at 180 BPM ≈ 27 seconds

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_2', 0.95));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_1', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Humanized timing - slight tempo variations to test beat stability tracking
 * Uses CONSISTENT samples with slight timing variations (±20ms)
 */
export const HUMANIZED_TIMING: TestPattern = {
  id: 'humanized-timing',
  name: 'Humanized Timing',
  description: 'Slight timing variations (±20ms) with consistent samples',
  durationMs: 20000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    let seed = 12345;
    const random = () => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return (seed / 0x7fffffff) - 0.5;
    };

    for (let beat = 0; beat < 40; beat++) {
      const baseTime = beatToTime(beat, bpm);
      const offset = random() * 0.040; // ±20ms
      const sampleId = beat % 2 === 0 ? 'kick_hard_2' : 'snare_hard_1';
      hits.push(deterministicHit(baseTime + offset, sampleId, 0.95));
    }
    return hits;
  })(),
};

/**
 * Perfect timing - metronomic precision for beat stability reference
 * Uses CONSISTENT samples with perfect timing
 */
export const PERFECT_TIMING: TestPattern = {
  id: 'perfect-timing',
  name: 'Perfect Timing',
  description: 'Metronomic precision with consistent samples - maximum beat stability',
  durationMs: 20000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 10; // 10 bars at 120 BPM = 20 seconds

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_2', 0.95));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_1', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Unstable timing - large tempo variations to test stability rejection
 * Uses CONSISTENT samples with large timing variations (±100ms)
 */
export const UNSTABLE_TIMING: TestPattern = {
  id: 'unstable-timing',
  name: 'Unstable Timing',
  description: 'Large timing variations (±100ms) with consistent samples',
  durationMs: 20000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    let seed = 54321;
    const random = () => {
      seed = (seed * 1103515245 + 12345) & 0x7fffffff;
      return (seed / 0x7fffffff) - 0.5;
    };

    for (let beat = 0; beat < 40; beat++) {
      const baseTime = beatToTime(beat, bpm);
      const offset = random() * 0.200; // ±100ms
      const sampleId = beat % 2 === 0 ? 'kick_hard_2' : 'snare_hard_1';
      hits.push(deterministicHit(baseTime + offset, sampleId, 0.95));
    }
    return hits;
  })(),
};

/**
 * Half-time ambiguity - tests hypothesis tracking of tempos that could be 60 or 120 BPM
 * Kicks only on beats 1 and 3 (of a 4/4 bar), creating ambiguity:
 * - Could be interpreted as 120 BPM (half note kicks)
 * - Could be interpreted as 60 BPM (quarter note kicks)
 * Tests that the multi-hypothesis tracker creates both hypotheses and selects the correct one.
 */
export const HALF_TIME_AMBIGUITY: TestPattern = {
  id: 'half-time-ambiguity',
  name: 'Half-Time Ambiguity',
  description: 'Kicks on 1 and 3 only - could be 60 or 120 BPM (tests multi-hypothesis tracking)',
  durationMs: 32000, // 32 seconds
  bpm: 120, // Actual BPM (but could be interpreted as 60 BPM)
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 16; // 16 bars at 120 BPM = 32 seconds

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4; // 4 beats per bar

      // Kick ONLY on beats 1 and 3 (half notes at 120 BPM, or quarter notes at 60 BPM)
      hits.push(hit(beatToTime(barOffset + 0, bpm), 'kick', 0.9));
      hits.push(hit(beatToTime(barOffset + 2, bpm), 'kick', 0.9));

      // No snare or hats - pure ambiguity
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

// =============================================================================
// EXTENDED BPM RANGE PATTERNS - Fill gaps at extreme tempos
// =============================================================================

/**
 * Steady 40 BPM - very slow tempo (ballad/ambient)
 * Tests minimum BPM detection capability
 */
export const STEADY_40BPM: TestPattern = {
  id: 'steady-40bpm',
  name: 'Steady 40 BPM',
  description: 'Very slow tempo - tests minimum BPM detection',
  durationMs: 36000, // 6 bars at 40 BPM = 36 seconds
  bpm: 40,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 40;
    const bars = 6;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 0.95));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Steady 50 BPM - slow tempo (slow ballad)
 */
export const STEADY_50BPM: TestPattern = {
  id: 'steady-50bpm',
  name: 'Steady 50 BPM',
  description: 'Slow tempo - tests low BPM detection',
  durationMs: 28800, // 6 bars at 50 BPM = 28.8 seconds
  bpm: 50,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 50;
    const bars = 6;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 0.95));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Steady 200 BPM - fast tempo (EDM/DnB)
 */
export const STEADY_200BPM: TestPattern = {
  id: 'steady-200bpm',
  name: 'Steady 200 BPM',
  description: 'Fast tempo - EDM/drum-n-bass range',
  durationMs: 24000, // 20 bars at 200 BPM = 24 seconds
  bpm: 200,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 200;
    const bars = 20;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_2', 0.95));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_1', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Steady 220 BPM - very fast tempo (liquid funk)
 */
export const STEADY_220BPM: TestPattern = {
  id: 'steady-220bpm',
  name: 'Steady 220 BPM',
  description: 'Very fast tempo - liquid funk range',
  durationMs: 21818, // 20 bars at 220 BPM ≈ 22 seconds
  bpm: 220,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 220;
    const bars = 20;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_2', 0.95));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_1', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Steady 240 BPM - extreme tempo (breakcore)
 */
export const STEADY_240BPM: TestPattern = {
  id: 'steady-240bpm',
  name: 'Steady 240 BPM',
  description: 'Extreme tempo - breakcore/speedcore range (maximum BPM test)',
  durationMs: 20000, // 20 bars at 240 BPM = 20 seconds
  bpm: 240,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 240;
    const bars = 20;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;
      // Use half-note pattern at extreme BPM
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'snare_hard_1', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

// =============================================================================
// POLYRHYTHM PATTERNS - Non-4/4 time signatures
// =============================================================================

/**
 * 3/4 Waltz - tests 3-beat cycle detection
 * Standard waltz pattern: kick on 1, snare on 2&3
 */
export const WALTZ_3_4: TestPattern = {
  id: 'waltz-3-4',
  name: 'Waltz 3/4 Time',
  description: '3/4 time signature - tests 3-beat cycle detection',
  durationMs: 24000, // 16 bars of 3/4 at 120 BPM = 24 seconds
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 16;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 3; // 3 beats per bar in 3/4

      // Kick on beat 1
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      // Snare on beats 2 and 3
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_medium_1', 0.7));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'snare_medium_2', 0.7));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * 5/4 Funk - tests 5-beat cycle (jazz/progressive)
 * Pattern: kick on 1&3, snare on 2&4&5
 */
export const FUNK_5_4: TestPattern = {
  id: 'funk-5-4',
  name: 'Funk 5/4 Time',
  description: '5/4 time signature - tests 5-beat cycle detection',
  durationMs: 25000, // 12 bars of 5/4 at 120 BPM = 25 seconds
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 12;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 5; // 5 beats per bar

      // Kick on beats 1 and 3
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 0.9));
      // Snare on beats 2, 4, and 5
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_medium_1', 0.75));
      hits.push(deterministicHit(beatToTime(barOffset + 4, bpm), 'snare_hard_2', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * 7/8 Progressive - tests 7-beat cycle (prog rock/metal)
 * Pattern: kick on 1&4, snare on 3&7
 */
export const PROG_7_8: TestPattern = {
  id: 'prog-7-8',
  name: 'Progressive 7/8 Time',
  description: '7/8 time signature - tests asymmetric meter detection',
  durationMs: 28000, // 16 bars of 7/8 at 120 BPM ≈ 28 seconds
  bpm: 120, // Each "beat" is an 8th note
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120; // 8th note = 120 BPM
    const bars = 16;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 3.5; // 7 eighth-notes = 3.5 quarter notes

      // Kick on 8ths 1 and 4
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 1.5, bpm), 'kick_hard_1', 0.9));
      // Snare on 8ths 3 and 7
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

// =============================================================================
// SWING/TRIPLET TIMING PATTERNS - Non-straight rhythmic feels
// =============================================================================

/**
 * Swing timing - classic jazz/funk swing feel
 * 8th notes are swung (66%/33% instead of 50%/50%)
 */
export const SWING_TIMING: TestPattern = {
  id: 'swing-timing',
  name: 'Swing Timing',
  description: 'Swing 8th notes (66%/33%) - tests non-straight timing',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;
    const swingRatio = 0.66; // 66% for downbeat, 33% for upbeat

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Kick on 1 and 3
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 0.95));

      // Snare on 2 and 4
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));

      // Swung hi-hats: downbeats normal, upbeats shifted late
      for (let beat = 0; beat < 4; beat++) {
        const downbeatTime = beatToTime(barOffset + beat, bpm);
        const upbeatTime = beatToTime(barOffset + beat + swingRatio * 0.5, bpm);
        hits.push(deterministicHit(downbeatTime, 'hat_medium_1', 0.6));
        hits.push(deterministicHit(upbeatTime, 'hat_soft_1', 0.4));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Triplet timing - triplet subdivision (12/8 feel)
 * 3 hits per beat instead of 2
 */
export const TRIPLET_TIMING: TestPattern = {
  id: 'triplet-timing',
  name: 'Triplet Timing',
  description: 'Triplet subdivision (12/8 feel) - tests 3-per-beat detection',
  durationMs: 16000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Kick on 1 and 3
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 0.95));

      // Snare on 2 and 4
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));

      // Triplet hi-hats: 3 hits per beat (at 0, 1/3, 2/3 of beat)
      for (let beat = 0; beat < 4; beat++) {
        for (let triplet = 0; triplet < 3; triplet++) {
          const tripletOffset = triplet / 3;
          const time = beatToTime(barOffset + beat + tripletOffset, bpm);
          const strength = triplet === 0 ? 0.7 : 0.4; // Accent on downbeat
          hits.push(deterministicHit(time, 'hat_medium_1', strength));
        }
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Shuffle backbeat - straight kick/snare with swung hats
 * Classic rock shuffle feel
 */
export const SHUFFLE_BACKBEAT: TestPattern = {
  id: 'shuffle-backbeat',
  name: 'Shuffle Backbeat',
  description: 'Straight drums with shuffle hats - classic rock feel',
  durationMs: 16000,
  bpm: 100,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 100;
    const bars = 8;
    const shuffleRatio = 0.67; // Triplet-based shuffle

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Straight kick on 1 and 3
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 0.95));

      // Straight snare on 2 and 4
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));

      // Shuffled hi-hats
      for (let beat = 0; beat < 4; beat++) {
        const downbeatTime = beatToTime(barOffset + beat, bpm);
        const shuffledUpbeat = beatToTime(barOffset + beat + shuffleRatio * 0.5, bpm);
        hits.push(deterministicHit(downbeatTime, 'hat_hard_1', 0.7));
        hits.push(deterministicHit(shuffledUpbeat, 'hat_soft_1', 0.45));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

// =============================================================================
// EXTREME TEMPO CHANGE PATTERNS
// =============================================================================

/**
 * Tempo jump extreme - large BPM changes (>50 BPM jumps)
 * Tests rapid tempo lock recovery
 */
export const TEMPO_JUMP_EXTREME: TestPattern = {
  id: 'tempo-jump-extreme',
  name: 'Tempo Jump Extreme',
  description: 'Large BPM jumps (160→80→180→100) - tests rapid recovery',
  durationMs: 24000,
  bpm: undefined,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const sections = [
      { bpm: 160, startTime: 0, duration: 6 },
      { bpm: 80, startTime: 6, duration: 6 },   // Drop to half
      { bpm: 180, startTime: 12, duration: 6 }, // Jump to 180
      { bpm: 100, startTime: 18, duration: 6 }, // Drop again
    ];

    for (const section of sections) {
      const beatsPerSecond = section.bpm / 60;
      const totalBeats = Math.floor(section.duration * beatsPerSecond);
      for (let beat = 0; beat < totalBeats; beat++) {
        const time = section.startTime + beat / beatsPerSecond;
        const sampleId = beat % 2 === 0 ? 'kick_hard_2' : 'snare_hard_1';
        hits.push(deterministicHit(time, sampleId, 0.95));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Tempo freeze - complete stop and restart
 * Tests deactivation and reactivation
 */
export const TEMPO_FREEZE: TestPattern = {
  id: 'tempo-freeze',
  name: 'Tempo Freeze',
  description: 'Music stops for 15s then resumes - tests deactivation/reactivation',
  durationMs: 40000,
  bpm: 120,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 120;

    // Section 1: 0-10s steady 120 BPM
    for (let beat = 0; beat < 20; beat++) {
      const sampleId = beat % 2 === 0 ? 'kick_hard_2' : 'snare_hard_1';
      hits.push(deterministicHit(beatToTime(beat, bpm), sampleId, 0.95));
    }

    // Gap: 10-25s (15 seconds of silence)

    // Section 2: 25-40s resume 120 BPM
    for (let beat = 0; beat < 30; beat++) {
      const sampleId = beat % 2 === 0 ? 'kick_hard_2' : 'snare_hard_1';
      hits.push(deterministicHit(25 + beatToTime(beat, bpm), sampleId, 0.95));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

// =============================================================================
// MELODIC/BASS-FOCUSED RHYTHM PATTERNS
// =============================================================================

/**
 * Bass groove - prominent bass line with sparse drums
 * Tests bass-driven rhythm detection
 */
export const BASS_GROOVE: TestPattern = {
  id: 'bass-groove',
  name: 'Bass Groove',
  description: 'Prominent bass line with sparse drums - tests bass-driven BPM',
  durationMs: 20000,
  bpm: 95,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 95;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Sparse kick on 1 only
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));

      // Snare only on beat 3
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'snare_hard_1', 0.9));

      // Prominent bass line - this should drive the rhythm
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'bass_hard_1', 0.85));
      hits.push(deterministicHit(beatToTime(barOffset + 0.75, bpm), 'bass_medium_1', 0.65));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'bass_hard_2', 0.8));
      hits.push(deterministicHit(beatToTime(barOffset + 1.5, bpm), 'bass_medium_2', 0.6));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'bass_hard_1', 0.85));
      hits.push(deterministicHit(beatToTime(barOffset + 2.75, bpm), 'bass_medium_1', 0.55));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'bass_hard_2', 0.75));
      hits.push(deterministicHit(beatToTime(barOffset + 3.5, bpm), 'bass_medium_2', 0.55));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Synth arpeggio rhythm - melodic arpeggios driving the rhythm
 * Tests if melodic transients contribute to BPM detection
 */
export const SYNTH_ARPEGGIO: TestPattern = {
  id: 'synth-arpeggio',
  name: 'Synth Arpeggio Rhythm',
  description: 'Melodic arpeggios driving rhythm - tests melodic BPM contribution',
  durationMs: 16000,
  bpm: 128,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 128;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Light drums (kick on 1, snare on 3)
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_medium_1', 0.75));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'snare_medium_1', 0.7));

      // 16th note arpeggio - should contribute strongly to rhythm
      for (let sixteenth = 0; sixteenth < 16; sixteenth++) {
        const time = beatToTime(barOffset + sixteenth * 0.25, bpm);
        const isAccent = sixteenth % 4 === 0;
        const sampleId = isAccent ? 'synth_stab_hard_1' : 'synth_stab_medium_1';
        const strength = isAccent ? 0.8 : 0.5;
        hits.push(deterministicHit(time, sampleId, strength));
      }
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * Chord rhythm - sustained chords with rhythmic attacks
 * Tests detection of chord attacks vs sustained portions
 */
export const CHORD_RHYTHM: TestPattern = {
  id: 'chord-rhythm',
  name: 'Chord Rhythm',
  description: 'Rhythmic chord stabs - tests chord attack detection',
  durationMs: 16000,
  bpm: 110,
  hits: (() => {
    const hits: GroundTruthHit[] = [];
    const bpm = 110;
    const bars = 8;

    for (let bar = 0; bar < bars; bar++) {
      const barOffset = bar * 4;

      // Kick and snare
      hits.push(deterministicHit(beatToTime(barOffset + 0, bpm), 'kick_hard_2', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 2, bpm), 'kick_hard_1', 0.9));
      hits.push(deterministicHit(beatToTime(barOffset + 1, bpm), 'snare_hard_1', 1.0));
      hits.push(deterministicHit(beatToTime(barOffset + 3, bpm), 'snare_hard_2', 1.0));

      // Rhythmic chord stabs (offbeat accents)
      hits.push(deterministicHit(beatToTime(barOffset + 0.5, bpm), 'synth_stab_hard_1', 0.8));
      hits.push(deterministicHit(beatToTime(barOffset + 1.5, bpm), 'synth_stab_medium_1', 0.6));
      hits.push(deterministicHit(beatToTime(barOffset + 2.5, bpm), 'synth_stab_hard_2', 0.8));
      hits.push(deterministicHit(beatToTime(barOffset + 3.5, bpm), 'synth_stab_medium_2', 0.6));
    }

    return hits.sort((a, b) => a.time - b.time);
  })(),
};

/**
 * All available test patterns
 */
export const TEST_PATTERNS: TestPattern[] = [
  // Calibrated patterns (deterministic, for precise measurement)
  STRONG_BEATS,
  MEDIUM_BEATS,
  SOFT_BEATS,
  HAT_REJECTION,
  MIXED_DYNAMICS,
  TEMPO_SWEEP,
  // Melodic/harmonic patterns (with bass, synth, lead)
  BASS_LINE,
  SYNTH_STABS,
  LEAD_MELODY,
  PAD_REJECTION,
  CHORD_REJECTION,
  FULL_MIX,
  // Legacy patterns (random samples, for variety testing)
  BASIC_DRUMS,
  KICK_FOCUS,
  SNARE_FOCUS,
  HAT_PATTERNS,
  FULL_KIT,
  SIMULTANEOUS_HITS,
  FAST_TEMPO,
  SPARSE_PATTERN,
  // Parameter-targeted patterns
  COOLDOWN_STRESS_20MS,
  COOLDOWN_STRESS_40MS,
  COOLDOWN_STRESS_80MS,
  THRESHOLD_GRADIENT,
  ATTACK_SHARP,
  ATTACK_GRADUAL,
  FREQ_LOW_ONLY,
  FREQ_HIGH_ONLY,
  // Music mode patterns
  STEADY_120BPM,
  STEADY_80BPM,
  STEADY_160BPM,
  STEADY_60BPM,
  STEADY_90BPM,
  STEADY_180BPM,
  TEMPO_RAMP,
  TEMPO_SUDDEN,
  PHASE_ON_BEAT,
  PHASE_OFF_BEAT,
  NON_MUSICAL_RANDOM,
  NON_MUSICAL_CLUSTERED,
  SILENCE_GAPS,
  HALF_TIME_AMBIGUITY,
  // Beat stability patterns
  PERFECT_TIMING,
  HUMANIZED_TIMING,
  UNSTABLE_TIMING,
  // Extended BPM range patterns
  STEADY_40BPM,
  STEADY_50BPM,
  STEADY_200BPM,
  STEADY_220BPM,
  STEADY_240BPM,
  // Polyrhythm patterns
  WALTZ_3_4,
  FUNK_5_4,
  PROG_7_8,
  // Swing/triplet timing patterns
  SWING_TIMING,
  TRIPLET_TIMING,
  SHUFFLE_BACKBEAT,
  // Extreme tempo change patterns
  TEMPO_JUMP_EXTREME,
  TEMPO_FREEZE,
  // Melodic/bass-focused rhythm patterns
  BASS_GROOVE,
  SYNTH_ARPEGGIO,
  CHORD_RHYTHM,
];

/**
 * Get pattern by ID
 */
export function getPatternById(id: string): TestPattern | undefined {
  return TEST_PATTERNS.find(p => p.id === id);
}

// =============================================================================
// PATTERN REGISTRY - Extended patterns with metadata for auto-discovery
// =============================================================================

/**
 * Helper to create metadata with sensible defaults for transient patterns
 */
function transientMeta(
  pattern: TestPattern,
  targetParams?: string[],
  priority: number = 5
): PatternMetadata {
  return {
    id: pattern.id,
    name: pattern.name,
    category: 'transient',
    targetParams,
    expectedMetrics: {
      primary: 'f1',
      secondary: ['precision', 'recall'],
    },
    expectedBpm: pattern.bpm,
    enabled: true,
    priority,
  };
}

/**
 * Helper to create metadata for rejection patterns
 */
function rejectionMeta(
  pattern: TestPattern,
  targetParams?: string[],
  priority: number = 5
): PatternMetadata {
  return {
    id: pattern.id,
    name: pattern.name,
    category: 'rejection',
    targetParams,
    expectedMetrics: {
      primary: 'precision',  // Rejection tests optimize for fewer false positives
      secondary: ['f1', 'rejection_rate'],
    },
    expectedBpm: pattern.bpm,
    enabled: true,
    priority,
  };
}

/**
 * Helper to create metadata for parameter-targeted patterns
 */
function paramTargetedMeta(
  pattern: TestPattern,
  targetParams: string[],
  priority: number = 7
): PatternMetadata {
  return {
    id: pattern.id,
    name: pattern.name,
    category: 'parameter-targeted',
    targetParams,
    expectedMetrics: {
      primary: 'f1',
      secondary: ['precision', 'recall'],
    },
    expectedBpm: pattern.bpm,
    enabled: true,
    priority,
  };
}

/**
 * Helper to create metadata for music mode patterns
 */
function musicModeMeta(
  pattern: TestPattern,
  targetParams?: string[],
  priority: number = 6
): PatternMetadata {
  return {
    id: pattern.id,
    name: pattern.name,
    category: 'music-mode',
    targetParams,
    expectedMetrics: {
      primary: 'bpm_accuracy',
      secondary: ['phase_stability', 'lock_time'],
    },
    expectedBpm: pattern.bpm,
    enabled: true,
    priority,
  };
}

/**
 * Wrap a TestPattern with metadata to create ExtendedTestPattern
 */
function extendPattern(pattern: TestPattern, metadata: PatternMetadata): ExtendedTestPattern {
  return { ...pattern, metadata };
}

/**
 * PATTERN REGISTRY - All patterns with metadata for extensibility
 *
 * EXTENSIBILITY: To add a new pattern:
 * 1. Create the TestPattern
 * 2. Add entry here with metadata
 * 3. System auto-discovers and includes in relevant tests
 */
export const PATTERN_REGISTRY: PatternRegistry = {
  // === CALIBRATED TRANSIENT PATTERNS ===

  'strong-beats': extendPattern(STRONG_BEATS, {
    ...transientMeta(STRONG_BEATS, ['hitthresh', 'fluxthresh', 'attackmult'], 10),
    // High priority baseline pattern
  }),

  'medium-beats': extendPattern(MEDIUM_BEATS, {
    ...transientMeta(MEDIUM_BEATS, ['hitthresh', 'fluxthresh'], 8),
  }),

  'soft-beats': extendPattern(SOFT_BEATS, {
    ...transientMeta(SOFT_BEATS, ['hitthresh', 'fluxthresh', 'attackmult'], 7),
  }),

  'hat-rejection': extendPattern(HAT_REJECTION, {
    ...rejectionMeta(HAT_REJECTION, ['hitthresh', 'fluxthresh'], 6),
  }),

  'mixed-dynamics': extendPattern(MIXED_DYNAMICS, {
    ...transientMeta(MIXED_DYNAMICS, ['hitthresh', 'attackmult'], 5),
  }),

  'tempo-sweep': extendPattern(TEMPO_SWEEP, {
    ...transientMeta(TEMPO_SWEEP, ['cooldown', 'bpmmin', 'bpmmax'], 7),
  }),

  // === MELODIC/HARMONIC PATTERNS ===

  'bass-line': extendPattern(BASS_LINE, {
    ...transientMeta(BASS_LINE, ['bassfreq', 'bassthresh', 'hitthresh'], 6),
  }),

  'synth-stabs': extendPattern(SYNTH_STABS, {
    ...transientMeta(SYNTH_STABS, ['hfcweight', 'hfcthresh', 'fluxthresh'], 6),
  }),

  'lead-melody': extendPattern(LEAD_MELODY, {
    ...transientMeta(LEAD_MELODY, ['fluxthresh', 'attackmult'], 5),
  }),

  // === REJECTION PATTERNS ===

  'pad-rejection': extendPattern(PAD_REJECTION, {
    ...rejectionMeta(PAD_REJECTION, ['fluxthresh', 'hitthresh', 'musicthresh'], 8),
  }),

  'chord-rejection': extendPattern(CHORD_REJECTION, {
    ...rejectionMeta(CHORD_REJECTION, ['fluxthresh', 'hitthresh'], 7),
  }),

  // === COMPLEX PATTERNS ===

  'full-mix': extendPattern(FULL_MIX, {
    ...transientMeta(FULL_MIX, ['hyfluxwt', 'hydrumwt', 'hybothboost'], 9),
  }),

  // === LEGACY PATTERNS ===

  'basic-drums': extendPattern(BASIC_DRUMS, {
    ...transientMeta(BASIC_DRUMS, undefined, 3),
  }),

  'kick-focus': extendPattern(KICK_FOCUS, {
    ...transientMeta(KICK_FOCUS, ['bassfreq', 'bassthresh'], 4),
  }),

  'snare-focus': extendPattern(SNARE_FOCUS, {
    ...transientMeta(SNARE_FOCUS, ['hfcweight', 'hfcthresh'], 4),
  }),

  'hat-patterns': extendPattern(HAT_PATTERNS, {
    ...transientMeta(HAT_PATTERNS, ['hitthresh', 'attackmult'], 3),
  }),

  'full-kit': extendPattern(FULL_KIT, {
    ...transientMeta(FULL_KIT, undefined, 3),
  }),

  'simultaneous': extendPattern(SIMULTANEOUS_HITS, {
    ...transientMeta(SIMULTANEOUS_HITS, ['cooldown', 'hybothboost'], 8),
  }),

  'fast-tempo': extendPattern(FAST_TEMPO, {
    ...transientMeta(FAST_TEMPO, ['cooldown', 'attackmult'], 9),
  }),

  'sparse': extendPattern(SPARSE_PATTERN, {
    ...rejectionMeta(SPARSE_PATTERN, ['musicthresh', 'hitthresh'], 6),
  }),

  // === PARAMETER-TARGETED PATTERNS ===

  'cooldown-stress-20ms': extendPattern(COOLDOWN_STRESS_20MS, {
    ...paramTargetedMeta(COOLDOWN_STRESS_20MS, ['cooldown'], 9),
  }),

  'cooldown-stress-40ms': extendPattern(COOLDOWN_STRESS_40MS, {
    ...paramTargetedMeta(COOLDOWN_STRESS_40MS, ['cooldown'], 9),
  }),

  'cooldown-stress-80ms': extendPattern(COOLDOWN_STRESS_80MS, {
    ...paramTargetedMeta(COOLDOWN_STRESS_80MS, ['cooldown'], 8),
  }),

  'threshold-gradient': extendPattern(THRESHOLD_GRADIENT, {
    ...paramTargetedMeta(THRESHOLD_GRADIENT, ['hitthresh', 'fluxthresh'], 8),
  }),

  'attack-sharp': extendPattern(ATTACK_SHARP, {
    ...paramTargetedMeta(ATTACK_SHARP, ['attackmult'], 7),
  }),

  'attack-gradual': extendPattern(ATTACK_GRADUAL, {
    ...rejectionMeta(ATTACK_GRADUAL, ['attackmult', 'fluxthresh'], 7),
  }),

  'freq-low-only': extendPattern(FREQ_LOW_ONLY, {
    ...paramTargetedMeta(FREQ_LOW_ONLY, ['bassfreq', 'bassthresh'], 6),
  }),

  'freq-high-only': extendPattern(FREQ_HIGH_ONLY, {
    ...paramTargetedMeta(FREQ_HIGH_ONLY, ['hfcweight', 'hfcthresh'], 6),
  }),

  // === MUSIC MODE PATTERNS ===

  'steady-120bpm': extendPattern(STEADY_120BPM, {
    ...musicModeMeta(STEADY_120BPM, ['musicthresh', 'bpmmin', 'bpmmax', 'pllkp', 'pllki'], 10),
  }),

  'steady-80bpm': extendPattern(STEADY_80BPM, {
    ...musicModeMeta(STEADY_80BPM, ['bpmmin', 'combdecay', 'combfb'], 8),
  }),

  'steady-160bpm': extendPattern(STEADY_160BPM, {
    ...musicModeMeta(STEADY_160BPM, ['bpmmax', 'cooldown'], 8),
  }),

  'tempo-ramp': extendPattern(TEMPO_RAMP, {
    ...musicModeMeta(TEMPO_RAMP, ['minpeakstr', 'promothresh', 'minbeats', 'bpmmatchtol'], 9),
  }),

  'tempo-sudden': extendPattern(TEMPO_SUDDEN, {
    ...musicModeMeta(TEMPO_SUDDEN, ['promothresh', 'minbeats', 'phrasehalf', 'minpeakstr'], 10),
  }),

  'phase-on-beat': extendPattern(PHASE_ON_BEAT, {
    ...musicModeMeta(PHASE_ON_BEAT, ['phasesnap', 'stablephase', 'confinc'], 9),
  }),

  'phase-off-beat': extendPattern(PHASE_OFF_BEAT, {
    ...musicModeMeta(PHASE_OFF_BEAT, ['phasesnap', 'snapconf'], 7),
  }),

  'non-musical-random': extendPattern(NON_MUSICAL_RANDOM, {
    ...musicModeMeta(NON_MUSICAL_RANDOM, ['musicthresh', 'musicbeats', 'minperiodicity'], 9),
  }),

  'non-musical-clustered': extendPattern(NON_MUSICAL_CLUSTERED, {
    ...musicModeMeta(NON_MUSICAL_CLUSTERED, ['musicthresh', 'beatthresh'], 8),
  }),

  'silence-gaps': extendPattern(SILENCE_GAPS, {
    ...musicModeMeta(SILENCE_GAPS, ['silencegrace', 'silencehalf', 'phrasehalf'], 9),
  }),

  'half-time-ambiguity': extendPattern(HALF_TIME_AMBIGUITY, {
    ...musicModeMeta(HALF_TIME_AMBIGUITY, ['minpeakstr', 'minrelheight', 'bpmmatchtol', 'promothresh'], 9),
  }),

  // === TEMPO PRIOR PATTERNS (new) ===

  'steady-60bpm': extendPattern(STEADY_60BPM, {
    ...musicModeMeta(STEADY_60BPM, ['priorenabled', 'priorcenter', 'priorwidth', 'priorstrength', 'bpmmin'], 9),
  }),

  'steady-90bpm': extendPattern(STEADY_90BPM, {
    ...musicModeMeta(STEADY_90BPM, ['priorenabled', 'priorcenter', 'priorwidth', 'priorstrength'], 8),
  }),

  'steady-180bpm': extendPattern(STEADY_180BPM, {
    ...musicModeMeta(STEADY_180BPM, ['priorenabled', 'priorcenter', 'priorwidth', 'priorstrength', 'bpmmax'], 9),
  }),

  // === BEAT STABILITY PATTERNS (new) ===

  'perfect-timing': extendPattern(PERFECT_TIMING, {
    ...musicModeMeta(PERFECT_TIMING, ['stabilitywin'], 10),
  }),

  'humanized-timing': extendPattern(HUMANIZED_TIMING, {
    ...musicModeMeta(HUMANIZED_TIMING, ['stabilitywin', 'temposmooth'], 8),
  }),

  'unstable-timing': extendPattern(UNSTABLE_TIMING, {
    ...musicModeMeta(UNSTABLE_TIMING, ['stabilitywin', 'temposmooth', 'tempochgthresh'], 9),
  }),

  // === EXTENDED BPM RANGE PATTERNS ===

  'steady-40bpm': extendPattern(STEADY_40BPM, {
    ...musicModeMeta(STEADY_40BPM, ['bpmmin', 'priorenabled', 'priorcenter', 'priorwidth'], 8),
  }),

  'steady-50bpm': extendPattern(STEADY_50BPM, {
    ...musicModeMeta(STEADY_50BPM, ['bpmmin', 'priorenabled', 'priorcenter', 'priorwidth'], 7),
  }),

  'steady-200bpm': extendPattern(STEADY_200BPM, {
    ...musicModeMeta(STEADY_200BPM, ['bpmmax', 'priorenabled', 'priorcenter', 'priorwidth', 'cooldown'], 8),
  }),

  'steady-220bpm': extendPattern(STEADY_220BPM, {
    ...musicModeMeta(STEADY_220BPM, ['bpmmax', 'cooldown'], 7),
  }),

  'steady-240bpm': extendPattern(STEADY_240BPM, {
    ...musicModeMeta(STEADY_240BPM, ['bpmmax', 'cooldown'], 7),
  }),

  // === POLYRHYTHM PATTERNS ===

  'waltz-3-4': extendPattern(WALTZ_3_4, {
    ...musicModeMeta(WALTZ_3_4, ['minpeakstr', 'promothresh'], 6),
  }),

  'funk-5-4': extendPattern(FUNK_5_4, {
    ...musicModeMeta(FUNK_5_4, ['minpeakstr', 'promothresh'], 6),
  }),

  'prog-7-8': extendPattern(PROG_7_8, {
    ...musicModeMeta(PROG_7_8, ['minpeakstr', 'promothresh'], 6),
  }),

  // === SWING/TRIPLET TIMING PATTERNS ===

  'swing-timing': extendPattern(SWING_TIMING, {
    ...musicModeMeta(SWING_TIMING, ['stabilitywin', 'temposmooth'], 7),
  }),

  'triplet-timing': extendPattern(TRIPLET_TIMING, {
    ...musicModeMeta(TRIPLET_TIMING, ['stabilitywin', 'temposmooth'], 7),
  }),

  'shuffle-backbeat': extendPattern(SHUFFLE_BACKBEAT, {
    ...musicModeMeta(SHUFFLE_BACKBEAT, ['stabilitywin', 'temposmooth'], 7),
  }),

  // === EXTREME TEMPO CHANGE PATTERNS ===

  'tempo-jump-extreme': extendPattern(TEMPO_JUMP_EXTREME, {
    ...musicModeMeta(TEMPO_JUMP_EXTREME, ['promothresh', 'minbeats', 'phrasehalf', 'temposmooth'], 9),
  }),

  'tempo-freeze': extendPattern(TEMPO_FREEZE, {
    ...musicModeMeta(TEMPO_FREEZE, ['silencegrace', 'silencehalf', 'phrasehalf'], 8),
  }),

  // === MELODIC/BASS-FOCUSED RHYTHM PATTERNS ===

  'bass-groove': extendPattern(BASS_GROOVE, {
    ...musicModeMeta(BASS_GROOVE, ['bassfreq', 'bassthresh', 'minpeakstr'], 8),
  }),

  'synth-arpeggio': extendPattern(SYNTH_ARPEGGIO, {
    ...musicModeMeta(SYNTH_ARPEGGIO, ['fluxthresh', 'hfcthresh', 'minpeakstr'], 7),
  }),

  'chord-rhythm': extendPattern(CHORD_RHYTHM, {
    ...musicModeMeta(CHORD_RHYTHM, ['fluxthresh', 'minpeakstr'], 7),
  }),
};

/**
 * Get extended pattern by ID (with metadata)
 */
export function getExtendedPatternById(id: string): ExtendedTestPattern | undefined {
  return PATTERN_REGISTRY[id];
}

/**
 * Get all patterns that target a specific parameter
 */
export function getPatternsForParam(param: string): ExtendedTestPattern[] {
  return Object.values(PATTERN_REGISTRY)
    .filter(p => p.metadata.enabled && p.metadata.targetParams?.includes(param))
    .sort((a, b) => (b.metadata.priority || 0) - (a.metadata.priority || 0));
}

/**
 * Get all enabled patterns in a category
 */
export function getPatternsByCategory(category: PatternCategory): ExtendedTestPattern[] {
  return Object.values(PATTERN_REGISTRY)
    .filter(p => p.metadata.enabled && p.metadata.category === category)
    .sort((a, b) => (b.metadata.priority || 0) - (a.metadata.priority || 0));
}
