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
import { INSTRUMENT_TO_BAND } from './types.js';
/**
 * Helper to convert BPM and beat number to time in seconds
 */
function beatToTime(beat, bpm) {
    const beatsPerSecond = bpm / 60;
    return beat / beatsPerSecond;
}
/**
 * Helper to create a hit with automatic band detection
 */
function hit(time, instrument, strength = 0.9) {
    return {
        time,
        type: INSTRUMENT_TO_BAND[instrument],
        instrument,
        strength,
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
/**
 * All available test patterns
 */
export const TEST_PATTERNS = [
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
