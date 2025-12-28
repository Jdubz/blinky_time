/**
 * Types for transient detection testing system
 */
/**
 * Mapping from instrument type to detection band
 */
export const INSTRUMENT_TO_BAND = {
    kick: 'low',
    tom: 'low',
    bass: 'low',
    snare: 'high',
    hat: 'high',
    clap: 'high',
    percussion: 'high',
    synth_stab: 'high', // Synth stabs are transient, high-frequency content
    lead: 'high', // Lead notes with attack are transient
    pad: 'low', // Pads are sustained, low frequency (but should NOT trigger)
    chord: 'high', // Chord stabs can have transient attack
};
/**
 * Whether an instrument type SHOULD trigger transient detection
 * - true: Has sharp attack, should be detected (kick, snare, synth_stab, etc.)
 * - false: Sustained/slow attack, should NOT trigger (pad, chord sustain)
 */
export const INSTRUMENT_SHOULD_TRIGGER = {
    kick: true,
    tom: true,
    bass: true, // Bass notes have attack - should trigger
    snare: true,
    hat: true, // Hats are transient but often quiet
    clap: true,
    percussion: true,
    synth_stab: true, // Sharp synth attack - should trigger
    lead: true, // Lead notes with attack - should trigger
    pad: false, // Sustained pad - should NOT trigger
    chord: false, // Sustained chord - should NOT trigger (stabs are different)
};
