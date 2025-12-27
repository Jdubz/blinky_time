/**
 * Web Audio API transient synthesizer
 *
 * Generates low-band (bass) and high-band (bright) transient sounds
 * programmatically without requiring audio file samples.
 */

import type { TransientType } from '../types/testTypes';

// Low-band (bass) synthesis constants - targets 50-200 Hz range
const LOW_START_FREQ = 150; // Hz - initial pitch
const LOW_END_FREQ = 50; // Hz - final pitch
const LOW_PITCH_BEND_TIME = 0.05; // seconds
const LOW_ATTACK_GAIN = 0.8; // 0.0-1.0
const LOW_DECAY_TIME = 0.5; // seconds
const LOW_MIN_GAIN = 0.01; // Minimum for exponential ramp

// Master synthesizer constants
const MASTER_VOLUME = 0.8; // 0.0-1.0

/**
 * Synthesize a low-band (bass) transient sound
 * Low-frequency sine wave (50-150Hz) with exponential decay
 * Returns the oscillator for cleanup tracking
 */
function synthLow(
  audioContext: AudioContext,
  destination: AudioNode,
  startTime: number,
  strength: number = 1.0
): OscillatorNode {
  // Oscillator for the bass "thump" - pitch drops from start to end frequency
  const osc = audioContext.createOscillator();
  const oscGain = audioContext.createGain();

  osc.type = 'sine';
  osc.frequency.setValueAtTime(LOW_START_FREQ, startTime);
  osc.frequency.exponentialRampToValueAtTime(LOW_END_FREQ, startTime + LOW_PITCH_BEND_TIME);

  // Amplitude envelope - quick attack, exponential decay
  oscGain.gain.setValueAtTime(LOW_ATTACK_GAIN * strength, startTime);
  oscGain.gain.exponentialRampToValueAtTime(LOW_MIN_GAIN, startTime + LOW_DECAY_TIME);

  osc.connect(oscGain);
  oscGain.connect(destination);

  osc.start(startTime);
  osc.stop(startTime + LOW_DECAY_TIME);

  return osc;
}

// Removed: synthHigh function - no longer needed with single-band detection

/**
 * Transient synthesizer class (kept as PercussionSynth for compatibility)
 * Manages AudioContext and provides simple interface for triggering sounds
 */
export class PercussionSynth {
  private audioContext: AudioContext;
  private masterGain: GainNode;
  private scheduledSources: Array<AudioScheduledSourceNode>;

  constructor() {
    this.audioContext = new AudioContext();
    this.masterGain = this.audioContext.createGain();
    this.masterGain.gain.value = MASTER_VOLUME;
    this.masterGain.connect(this.audioContext.destination);
    this.scheduledSources = [];
  }

  /**
   * Trigger a transient sound at specified absolute time
   * @param _type - Transient type (unused - single-band detection)
   * @param audioTime - Absolute time in AudioContext timeline (seconds)
   * @param strength - Hit strength 0.0-1.0
   */
  trigger(_type: TransientType, audioTime: number, strength: number = 1.0): void {
    // Simplified single-band detection - always use low-band synthesis
    const osc = synthLow(this.audioContext, this.masterGain, audioTime, strength);
    this.scheduledSources.push(osc);
  }

  /**
   * Get current AudioContext time
   */
  getCurrentTime(): number {
    return this.audioContext.currentTime;
  }

  /**
   * Resume AudioContext (required after user interaction in some browsers)
   */
  async resume(): Promise<void> {
    if (this.audioContext.state === 'suspended') {
      await this.audioContext.resume();
    }
  }

  /**
   * Stop all audio playback immediately by muting master gain and canceling scheduled sounds
   */
  stop(): void {
    // Stop and disconnect all scheduled audio sources
    for (const source of this.scheduledSources) {
      try {
        source.stop();
        source.disconnect();
      } catch {
        // Source may have already stopped naturally, ignore error
      }
    }
    this.scheduledSources = [];

    // Immediately cut off all audio
    this.masterGain.gain.setValueAtTime(0, this.audioContext.currentTime);
  }

  /**
   * Resume audio playback by restoring master gain and clearing source list
   */
  start(): void {
    // Clear any previous scheduled sources
    this.scheduledSources = [];
    // Restore master volume
    this.masterGain.gain.setValueAtTime(MASTER_VOLUME, this.audioContext.currentTime);
  }

  /**
   * Clean up resources
   */
  dispose(): void {
    this.audioContext.close();
  }
}
