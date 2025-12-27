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
/**
 * Helper to convert BPM and beat number to time in seconds
 */
function beatToTime(beat, bpm) {
    const beatsPerSecond = bpm / 60;
    return beat / beatsPerSecond;
}
/**
 * Simple alternating pattern (120 BPM, 8 bars)
 * Low transients on 1 and 3, high transients on 2 and 4
 */
export const SIMPLE_BEAT = {
    id: 'simple-beat',
    name: 'Alternating Low/High',
    description: 'Low on 1&3, high on 2&4, alternating (120 BPM, 8 bars)',
    durationMs: 16000, // 8 bars at 120 BPM
    bpm: 120,
    hits: (() => {
        const hits = [];
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
export const LOW_BARRAGE = {
    id: 'low-barrage',
    name: 'Low Band Barrage',
    description: 'Rapid bass transients at varying intervals - tests low-band detection accuracy',
    durationMs: 8000,
    hits: (() => {
        const hits = [];
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
export const HIGH_BURST = {
    id: 'high-burst',
    name: 'High Band Burst',
    description: 'Rapid high-frequency transients - tests high-band detection accuracy',
    durationMs: 6000,
    hits: (() => {
        const hits = [];
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
export const MIXED_PATTERN = {
    id: 'mixed-pattern',
    name: 'Mixed Low/High',
    description: 'Interleaved low and high transients with varying dynamics',
    durationMs: 10000,
    bpm: 100,
    hits: (() => {
        const hits = [];
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
export const TIMING_TEST = {
    id: 'timing-test',
    name: 'Timing Precision Test',
    description: 'Transients at 100ms, 150ms, 200ms, 250ms intervals - tests timing accuracy',
    durationMs: 10000,
    hits: (() => {
        const hits = [];
        const intervals = [0.1, 0.15, 0.2, 0.25]; // Seconds
        for (const interval of intervals) {
            const sectionStart = hits.length > 0 ? hits[hits.length - 1].time + 0.5 : 0.5;
            for (let i = 0; i < 10; i++) {
                // Alternate between low and high
                const types = ['low', 'high'];
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
export const SIMULTANEOUS_TEST = {
    id: 'simultaneous',
    name: 'Simultaneous Hits',
    description: 'Low and high transients at exactly the same time - tests concurrent detection',
    durationMs: 8000,
    hits: (() => {
        const hits = [];
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
 * Realistic electronic track simulation
 * Background: sub-bass drone + mid pad + noise floor
 * This tests detection in the presence of continuous audio, like real music
 */
export const REALISTIC_TRACK = {
    id: 'realistic-track',
    name: 'Realistic Electronic Track',
    description: 'Kick/hi-hat pattern with background audio (sub-bass, pad, noise floor)',
    durationMs: 16000,
    bpm: 128,
    background: {
        lowDrone: { frequency: 55, gain: 0.15 }, // A1 sub-bass
        midPad: { frequency: 220, gain: 0.08 }, // A3 pad
        noiseFloor: { gain: 0.03 }, // Subtle hi-hat bleed
    },
    hits: (() => {
        const hits = [];
        const bpm = 128;
        const bars = 8;
        for (let bar = 0; bar < bars; bar++) {
            const barOffset = bar * 4;
            // Kick on 1 and 3
            hits.push({ time: beatToTime(barOffset + 0, bpm), type: 'low', strength: 0.9 });
            hits.push({ time: beatToTime(barOffset + 2, bpm), type: 'low', strength: 0.9 });
            // Open hi-hat on offbeats (8th notes)
            for (let i = 0; i < 4; i++) {
                hits.push({ time: beatToTime(barOffset + i + 0.5, bpm), type: 'high', strength: 0.7 });
            }
        }
        return hits.sort((a, b) => a.time - b.time);
    })(),
};
/**
 * Heavy background test
 * High background levels to test detection sensitivity in loud environments
 */
export const HEAVY_BACKGROUND = {
    id: 'heavy-background',
    name: 'Heavy Background',
    description: 'Transients over loud continuous audio - tests detection in high-energy environment',
    durationMs: 10000,
    bpm: 140,
    background: {
        lowDrone: { frequency: 60, gain: 0.25 }, // Loud sub-bass
        midPad: { frequency: 300, gain: 0.15 }, // Loud pad
        noiseFloor: { gain: 0.08 }, // Significant noise floor
    },
    hits: (() => {
        const hits = [];
        const bpm = 140;
        // Sparse but strong transients
        for (let beat = 0; beat < 20; beat += 2) {
            hits.push({ time: beatToTime(beat, bpm), type: 'low', strength: 1.0 });
            hits.push({ time: beatToTime(beat + 1, bpm), type: 'high', strength: 1.0 });
        }
        return hits.filter(h => h.time < 10.0);
    })(),
};
/**
 * Dynamic background test
 * No transients, just background - for observing baseline adaptation
 */
export const BASELINE_ONLY = {
    id: 'baseline-only',
    name: 'Baseline Only (No Transients)',
    description: 'Only background audio, no transients - observe baseline behavior',
    durationMs: 8000,
    background: {
        lowDrone: { frequency: 80, gain: 0.2 },
        midPad: { frequency: 250, gain: 0.1 },
        noiseFloor: { gain: 0.05 },
    },
    hits: [], // No transients - any detections are false positives
};
/**
 * Quiet section test
 * Simulates a breakdown/quiet section in a track
 */
export const QUIET_SECTION = {
    id: 'quiet-section',
    name: 'Quiet Section',
    description: 'Low-level background with subtle transients - tests sensitivity',
    durationMs: 12000,
    bpm: 100,
    background: {
        lowDrone: { frequency: 50, gain: 0.05 }, // Very quiet sub-bass
        noiseFloor: { gain: 0.01 }, // Minimal noise
    },
    hits: (() => {
        const hits = [];
        const bpm = 100;
        // Soft, sparse transients
        for (let beat = 0; beat < 16; beat += 2) {
            hits.push({ time: beatToTime(beat, bpm), type: 'low', strength: 0.5 });
            if (beat % 4 === 0) {
                hits.push({ time: beatToTime(beat + 1.5, bpm), type: 'high', strength: 0.4 });
            }
        }
        return hits.filter(h => h.time < 12.0);
    })(),
};
/**
 * All available test patterns
 */
export const TEST_PATTERNS = [
    SIMPLE_BEAT,
    LOW_BARRAGE,
    HIGH_BURST,
    MIXED_PATTERN,
    TIMING_TEST,
    SIMULTANEOUS_TEST,
    // Realistic patterns with background audio
    REALISTIC_TRACK,
    HEAVY_BACKGROUND,
    BASELINE_ONLY,
    QUIET_SECTION,
];
/**
 * Get pattern by ID
 */
export function getPatternById(id) {
    return TEST_PATTERNS.find(p => p.id === id);
}
