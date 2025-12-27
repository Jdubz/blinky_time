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
import type { TestPattern } from './types.js';
/**
 * Basic drum pattern (120 BPM, 8 bars)
 * Kick on 1 and 3, snare on 2 and 4, hats on 8th notes
 */
export declare const BASIC_DRUMS: TestPattern;
/**
 * Kick focus pattern - various kick patterns
 * Tests low-band detection accuracy
 */
export declare const KICK_FOCUS: TestPattern;
/**
 * Snare focus pattern
 * Tests high-band detection with snare variations
 */
export declare const SNARE_FOCUS: TestPattern;
/**
 * Hi-hat patterns
 * Tests high-band detection with hat variations
 */
export declare const HAT_PATTERNS: TestPattern;
/**
 * Full kit pattern - kick, snare, hat, tom, clap
 * Tests all instrument types together
 */
export declare const FULL_KIT: TestPattern;
/**
 * Simultaneous hits - kick + snare, kick + clap
 * Tests detection when multiple bands trigger at once
 */
export declare const SIMULTANEOUS_HITS: TestPattern;
/**
 * Fast tempo test (150 BPM)
 * Tests detection at higher speeds
 */
export declare const FAST_TEMPO: TestPattern;
/**
 * Sparse pattern - widely spaced hits
 * Tests detection with long gaps between transients
 */
export declare const SPARSE_PATTERN: TestPattern;
/**
 * CALIBRATED: Strong beats only (120 BPM, 8 bars)
 * Uses only hard kick and snare samples - should be easy to detect
 * Expected: ~100% recall with any reasonable threshold
 */
export declare const STRONG_BEATS: TestPattern;
/**
 * CALIBRATED: Medium beats (120 BPM, 8 bars)
 * Uses medium kick and snare samples - moderate detection challenge
 */
export declare const MEDIUM_BEATS: TestPattern;
/**
 * CALIBRATED: Soft beats (120 BPM, 8 bars)
 * Uses soft kick and snare samples - difficult detection, tests sensitivity
 */
export declare const SOFT_BEATS: TestPattern;
/**
 * CALIBRATED: Hat rejection test (120 BPM, 8 bars)
 * Hard kicks/snares with soft hi-hats - tests ability to ignore weak transients
 * Expected: Detect kicks/snares, reject hi-hats
 */
export declare const HAT_REJECTION: TestPattern;
/**
 * CALIBRATED: Mixed dynamics (120 BPM, 8 bars)
 * Realistic pattern with varying loudness - simulates real music
 */
export declare const MIXED_DYNAMICS: TestPattern;
/**
 * CALIBRATED: Tempo sweep (4 bars each at 80, 100, 120, 140 BPM)
 * Tests detection across tempo range
 */
export declare const TEMPO_SWEEP: TestPattern;
/**
 * All available test patterns
 */
export declare const TEST_PATTERNS: TestPattern[];
/**
 * Get pattern by ID
 */
export declare function getPatternById(id: string): TestPattern | undefined;
