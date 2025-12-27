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
 * All available test patterns
 */
export declare const TEST_PATTERNS: TestPattern[];
/**
 * Get pattern by ID
 */
export declare function getPatternById(id: string): TestPattern | undefined;
