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
import type { TestPattern, ExtendedTestPattern, PatternRegistry, PatternCategory } from './types.js';
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
 * CALIBRATED: Bass line with kicks (120 BPM, 8 bars)
 * Tests bass note detection alongside kicks
 * Expected: Both kicks AND bass notes should trigger (both are transients)
 */
export declare const BASS_LINE: TestPattern;
/**
 * CALIBRATED: Synth stab pattern (120 BPM, 8 bars)
 * Sharp synth stabs that SHOULD trigger transient detection
 */
export declare const SYNTH_STABS: TestPattern;
/**
 * CALIBRATED: Lead melody with drum accents (100 BPM, 8 bars)
 * Tests if lead note attacks are detected alongside drums
 */
export declare const LEAD_MELODY: TestPattern;
/**
 * CALIBRATED: Pad rejection test (80 BPM, 8 bars)
 * Sustained pads playing throughout with sparse drum hits
 * Pads should NOT trigger - only drums should be detected
 * Tests false positive rejection for sustained sounds
 */
export declare const PAD_REJECTION: TestPattern;
/**
 * CALIBRATED: Chord rejection test (90 BPM, 8 bars)
 * Sustained chords playing with drum hits
 * Chords should NOT trigger - only drums should be detected
 */
export declare const CHORD_REJECTION: TestPattern;
/**
 * CALIBRATED: Full mix (120 BPM, 8 bars)
 * Realistic EDM-style mix with drums, bass, synths, leads
 * Tests detection in complex, layered audio
 */
export declare const FULL_MIX: TestPattern;
/**
 * Cooldown stress test - hits every 20ms
 * Tests minimum cooldown capability
 */
export declare const COOLDOWN_STRESS_20MS: TestPattern;
/**
 * Cooldown stress test - hits every 40ms
 * Tests moderate cooldown
 */
export declare const COOLDOWN_STRESS_40MS: TestPattern;
/**
 * Cooldown stress test - hits every 80ms
 * Tests standard cooldown
 */
export declare const COOLDOWN_STRESS_80MS: TestPattern;
/**
 * Threshold gradient - hits with decreasing strength
 * Tests hitthresh and fluxthresh sensitivity
 */
export declare const THRESHOLD_GRADIENT: TestPattern;
/**
 * Sharp attack patterns - tests attackmult sensitivity to fast transients
 */
export declare const ATTACK_SHARP: TestPattern;
/**
 * Gradual attack patterns - slower envelope swells (should NOT trigger)
 */
export declare const ATTACK_GRADUAL: TestPattern;
/**
 * Low frequency only - tests bass detection
 */
export declare const FREQ_LOW_ONLY: TestPattern;
/**
 * High frequency only - tests HFC detection
 */
export declare const FREQ_HIGH_ONLY: TestPattern;
/**
 * Steady 120 BPM - baseline for BPM tracking
 * Alternates between sample variants for realistic phrase structure
 */
export declare const STEADY_120BPM: TestPattern;
/**
 * Steady 80 BPM - slow tempo tracking
 * Alternates between sample variants for realistic phrase structure
 */
export declare const STEADY_80BPM: TestPattern;
/**
 * Steady 160 BPM - fast tempo tracking
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export declare const STEADY_160BPM: TestPattern;
/**
 * Tempo ramp - gradual BPM change from 80 to 160
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export declare const TEMPO_RAMP: TestPattern;
/**
 * Tempo sudden - abrupt BPM changes
 */
export declare const TEMPO_SUDDEN: TestPattern;
/**
 * Phase on-beat - hits exactly on expected beats
 */
export declare const PHASE_ON_BEAT: TestPattern;
/**
 * Phase off-beat - syncopated hits
 */
export declare const PHASE_OFF_BEAT: TestPattern;
/**
 * Non-musical random - random timing for music mode rejection
 */
export declare const NON_MUSICAL_RANDOM: TestPattern;
/**
 * Non-musical clustered - irregular clusters
 */
export declare const NON_MUSICAL_CLUSTERED: TestPattern;
/**
 * Silence gaps - tests music mode deactivation
 */
export declare const SILENCE_GAPS: TestPattern;
/**
 * Steady 60 BPM - tests tempo prior's subharmonic rejection
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export declare const STEADY_60BPM: TestPattern;
/**
 * Steady 90 BPM - intermediate tempo
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export declare const STEADY_90BPM: TestPattern;
/**
 * Steady 180 BPM - fast tempo subharmonic test
 * Uses CONSISTENT samples for reliable autocorrelation
 */
export declare const STEADY_180BPM: TestPattern;
/**
 * Humanized timing - slight tempo variations to test beat stability tracking
 * Uses CONSISTENT samples with slight timing variations (±20ms)
 */
export declare const HUMANIZED_TIMING: TestPattern;
/**
 * Perfect timing - metronomic precision for beat stability reference
 * Uses CONSISTENT samples with perfect timing
 */
export declare const PERFECT_TIMING: TestPattern;
/**
 * Unstable timing - large tempo variations to test stability rejection
 * Uses CONSISTENT samples with large timing variations (±100ms)
 */
export declare const UNSTABLE_TIMING: TestPattern;
/**
 * Half-time ambiguity - tests hypothesis tracking of tempos that could be 60 or 120 BPM
 * Kicks only on beats 1 and 3 (of a 4/4 bar), creating ambiguity:
 * - Could be interpreted as 120 BPM (half note kicks)
 * - Could be interpreted as 60 BPM (quarter note kicks)
 * Tests that the multi-hypothesis tracker creates both hypotheses and selects the correct one.
 */
export declare const HALF_TIME_AMBIGUITY: TestPattern;
/**
 * Steady 40 BPM - very slow tempo (ballad/ambient)
 * Tests minimum BPM detection capability
 */
export declare const STEADY_40BPM: TestPattern;
/**
 * Steady 50 BPM - slow tempo (slow ballad)
 */
export declare const STEADY_50BPM: TestPattern;
/**
 * Steady 200 BPM - fast tempo (EDM/DnB)
 */
export declare const STEADY_200BPM: TestPattern;
/**
 * Steady 220 BPM - very fast tempo (liquid funk)
 */
export declare const STEADY_220BPM: TestPattern;
/**
 * Steady 240 BPM - extreme tempo (breakcore)
 */
export declare const STEADY_240BPM: TestPattern;
/**
 * 3/4 Waltz - tests 3-beat cycle detection
 * Standard waltz pattern: kick on 1, snare on 2&3
 */
export declare const WALTZ_3_4: TestPattern;
/**
 * 5/4 Funk - tests 5-beat cycle (jazz/progressive)
 * Pattern: kick on 1&3, snare on 2&4&5
 */
export declare const FUNK_5_4: TestPattern;
/**
 * 7/8 Progressive - tests 7-beat cycle (prog rock/metal)
 * Pattern: kick on 1&4, snare on 3&7
 */
export declare const PROG_7_8: TestPattern;
/**
 * Swing timing - classic jazz/funk swing feel
 * 8th notes are swung (66%/33% instead of 50%/50%)
 */
export declare const SWING_TIMING: TestPattern;
/**
 * Triplet timing - triplet subdivision (12/8 feel)
 * 3 hits per beat instead of 2
 */
export declare const TRIPLET_TIMING: TestPattern;
/**
 * Shuffle backbeat - straight kick/snare with swung hats
 * Classic rock shuffle feel
 */
export declare const SHUFFLE_BACKBEAT: TestPattern;
/**
 * Tempo jump extreme - large BPM changes (>50 BPM jumps)
 * Tests rapid tempo lock recovery
 */
export declare const TEMPO_JUMP_EXTREME: TestPattern;
/**
 * Tempo freeze - complete stop and restart
 * Tests deactivation and reactivation
 */
export declare const TEMPO_FREEZE: TestPattern;
/**
 * Bass groove - prominent bass line with sparse drums
 * Tests bass-driven rhythm detection
 */
export declare const BASS_GROOVE: TestPattern;
/**
 * Synth arpeggio rhythm - melodic arpeggios driving the rhythm
 * Tests if melodic transients contribute to BPM detection
 */
export declare const SYNTH_ARPEGGIO: TestPattern;
/**
 * Chord rhythm - sustained chords with rhythmic attacks
 * Tests detection of chord attacks vs sustained portions
 */
export declare const CHORD_RHYTHM: TestPattern;
/**
 * All available test patterns
 */
export declare const TEST_PATTERNS: TestPattern[];
/**
 * Get pattern by ID
 */
export declare function getPatternById(id: string): TestPattern | undefined;
/**
 * PATTERN REGISTRY - All patterns with metadata for extensibility
 *
 * EXTENSIBILITY: To add a new pattern:
 * 1. Create the TestPattern
 * 2. Add entry here with metadata
 * 3. System auto-discovers and includes in relevant tests
 */
export declare const PATTERN_REGISTRY: PatternRegistry;
/**
 * Get extended pattern by ID (with metadata)
 */
export declare function getExtendedPatternById(id: string): ExtendedTestPattern | undefined;
/**
 * Get all patterns that target a specific parameter
 */
export declare function getPatternsForParam(param: string): ExtendedTestPattern[];
/**
 * Get all enabled patterns in a category
 */
export declare function getPatternsByCategory(category: PatternCategory): ExtendedTestPattern[];
