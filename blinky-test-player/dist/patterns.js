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
import { INSTRUMENT_TO_BAND, INSTRUMENT_SHOULD_TRIGGER } from './types.js';
/**
 * Helper to convert BPM and beat number to time in seconds
 */
function beatToTime(beat, bpm) {
    const beatsPerSecond = bpm / 60;
    return beat / beatsPerSecond;
}
/**
 * Helper to create a hit with automatic band detection and trigger expectation
 */
function hit(time, instrument, strength = 0.9) {
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
export const BASIC_DRUMS = {
    id: 'basic-drums',
    name: 'Basic Drum Pattern',
    description: 'Kick on 1&3, snare on 2&4, hats on 8th notes (120 BPM, 8 bars)',
    durationMs: 16000,
    bpm: 120,
    hits: (() => {
        const hits = [];
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
export const KICK_FOCUS = {
    id: 'kick-focus',
    name: 'Kick Focus',
    description: 'Various kick patterns at different intervals - tests low-band detection',
    durationMs: 12000,
    bpm: 100,
    hits: (() => {
        const hits = [];
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
export const SNARE_FOCUS = {
    id: 'snare-focus',
    name: 'Snare Focus',
    description: 'Various snare patterns including rolls - tests high-band detection',
    durationMs: 10000,
    bpm: 110,
    hits: (() => {
        const hits = [];
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
export const HAT_PATTERNS = {
    id: 'hat-patterns',
    name: 'Hi-Hat Patterns',
    description: 'Various hi-hat patterns: 8ths, 16ths, offbeats',
    durationMs: 12000,
    bpm: 125,
    hits: (() => {
        const hits = [];
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
export const FULL_KIT = {
    id: 'full-kit',
    name: 'Full Drum Kit',
    description: 'All drum elements: kick, snare, hat, tom, clap',
    durationMs: 16000,
    bpm: 115,
    hits: (() => {
        const hits = [];
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
export const SIMULTANEOUS_HITS = {
    id: 'simultaneous',
    name: 'Simultaneous Hits',
    description: 'Kick + snare/clap at same time - tests concurrent detection',
    durationMs: 10000,
    bpm: 100,
    hits: (() => {
        const hits = [];
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
export const FAST_TEMPO = {
    id: 'fast-tempo',
    name: 'Fast Tempo (150 BPM)',
    description: 'High-speed drum pattern - tests detection at fast tempos',
    durationMs: 10000,
    bpm: 150,
    hits: (() => {
        const hits = [];
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
export const SPARSE_PATTERN = {
    id: 'sparse',
    name: 'Sparse Pattern',
    description: 'Widely spaced hits - tests detection after silence periods',
    durationMs: 15000,
    hits: (() => {
        const hits = [];
        // Hits at irregular, wide intervals
        const times = [0.5, 2.0, 3.5, 6.0, 8.0, 9.5, 12.0, 14.0];
        const instruments = ['kick', 'snare', 'kick', 'tom', 'kick', 'clap', 'snare', 'kick'];
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
function deterministicHit(time, sampleId, strength = 1.0) {
    // Extract type from sampleId (e.g., "kick_hard_1" -> "kick", "synth_stab_hard_1" -> "synth_stab")
    const parts = sampleId.split('_');
    // Handle compound types like "synth_stab" - check if second part is a loudness level
    const loudnessLevels = ['hard', 'medium', 'soft', 'slow'];
    let type;
    if (parts.length >= 3 && loudnessLevels.includes(parts[1])) {
        type = parts[0]; // Simple type like "kick"
    }
    else if (parts.length >= 3) {
        type = parts.slice(0, 2).join('_'); // Compound type like "synth_stab"
    }
    else {
        type = parts[0];
    }
    const instrument = type;
    return {
        time,
        type: INSTRUMENT_TO_BAND[instrument] || 'high',
        instrument,
        strength,
        sampleId, // New field for deterministic selection
        expectTrigger: INSTRUMENT_SHOULD_TRIGGER[instrument] ?? true,
    };
}
/**
 * CALIBRATED: Strong beats only (120 BPM, 8 bars)
 * Uses only hard kick and snare samples - should be easy to detect
 * Expected: ~100% recall with any reasonable threshold
 */
export const STRONG_BEATS = {
    id: 'strong-beats',
    name: 'Strong Beats (Calibrated)',
    description: 'Hard kicks and snares only - baseline detection test',
    durationMs: 16000,
    bpm: 120,
    hits: (() => {
        const hits = [];
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
export const MEDIUM_BEATS = {
    id: 'medium-beats',
    name: 'Medium Beats (Calibrated)',
    description: 'Medium loudness kicks and snares - moderate detection challenge',
    durationMs: 16000,
    bpm: 120,
    hits: (() => {
        const hits = [];
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
export const SOFT_BEATS = {
    id: 'soft-beats',
    name: 'Soft Beats (Calibrated)',
    description: 'Soft kicks and snares - tests detection sensitivity limits',
    durationMs: 16000,
    bpm: 120,
    hits: (() => {
        const hits = [];
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
export const HAT_REJECTION = {
    id: 'hat-rejection',
    name: 'Hat Rejection (Calibrated)',
    description: 'Hard kicks/snares + soft hats - tests hi-hat rejection',
    durationMs: 16000,
    bpm: 120,
    hits: (() => {
        const hits = [];
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
export const MIXED_DYNAMICS = {
    id: 'mixed-dynamics',
    name: 'Mixed Dynamics (Calibrated)',
    description: 'Varying loudness pattern - realistic music simulation',
    durationMs: 16000,
    bpm: 120,
    hits: (() => {
        const hits = [];
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
export const TEMPO_SWEEP = {
    id: 'tempo-sweep',
    name: 'Tempo Sweep (Calibrated)',
    description: 'Tests detection at 80, 100, 120, 140 BPM',
    durationMs: 16000,
    hits: (() => {
        const hits = [];
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
export const BASS_LINE = {
    id: 'bass-line',
    name: 'Bass Line (Calibrated)',
    description: 'Kicks + bass notes - tests low frequency transient detection',
    durationMs: 16000,
    bpm: 120,
    hits: (() => {
        const hits = [];
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
export const SYNTH_STABS = {
    id: 'synth-stabs',
    name: 'Synth Stabs (Calibrated)',
    description: 'Sharp synth stabs - should trigger transient detection',
    durationMs: 16000,
    bpm: 120,
    hits: (() => {
        const hits = [];
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
export const LEAD_MELODY = {
    id: 'lead-melody',
    name: 'Lead Melody (Calibrated)',
    description: 'Lead notes + drums - tests melodic transient detection',
    durationMs: 19200, // 8 bars at 100 BPM
    bpm: 100,
    hits: (() => {
        const hits = [];
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
export const PAD_REJECTION = {
    id: 'pad-rejection',
    name: 'Pad Rejection (Calibrated)',
    description: 'Sustained pads + sparse drums - pads should NOT trigger',
    durationMs: 24000, // 8 bars at 80 BPM
    bpm: 80,
    hits: (() => {
        const hits = [];
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
export const CHORD_REJECTION = {
    id: 'chord-rejection',
    name: 'Chord Rejection (Calibrated)',
    description: 'Sustained chords + drums - chords should NOT trigger',
    durationMs: 21333, // 8 bars at 90 BPM
    bpm: 90,
    hits: (() => {
        const hits = [];
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
export const FULL_MIX = {
    id: 'full-mix',
    name: 'Full Mix (Calibrated)',
    description: 'Drums + bass + synth + lead - realistic music simulation',
    durationMs: 16000,
    bpm: 120,
    hits: (() => {
        const hits = [];
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
/**
 * All available test patterns
 */
export const TEST_PATTERNS = [
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
];
/**
 * Get pattern by ID
 */
export function getPatternById(id) {
    return TEST_PATTERNS.find(p => p.id === id);
}
