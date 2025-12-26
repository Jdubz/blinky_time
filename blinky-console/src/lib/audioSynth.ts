/**
 * Web Audio API percussion synthesizer
 *
 * Generates kick, snare, and hi-hat sounds programmatically
 * without requiring audio file samples.
 */

import type { PercussionType } from '../types/testTypes';

// Kick drum synthesis constants
const KICK_START_FREQ = 150; // Hz - initial pitch
const KICK_END_FREQ = 50; // Hz - final pitch
const KICK_PITCH_BEND_TIME = 0.05; // seconds
const KICK_ATTACK_GAIN = 0.8; // 0.0-1.0
const KICK_DECAY_TIME = 0.5; // seconds
const KICK_MIN_GAIN = 0.01; // Minimum for exponential ramp

// Snare drum synthesis constants
const SNARE_DURATION = 0.2; // seconds
const SNARE_NOISE_GAIN = 0.7; // 0.0-1.0
const SNARE_TONE_GAIN = 0.3; // 0.0-1.0
const SNARE_TONE_FREQ = 180; // Hz - shell resonance
const SNARE_TONE_DECAY = 0.1; // seconds
const SNARE_FILTER_FREQ = 1000; // Hz - highpass cutoff
const SNARE_MIN_GAIN = 0.01; // Minimum for exponential ramp

// Hi-hat synthesis constants
const HIHAT_DURATION = 0.05; // seconds
const HIHAT_GAIN = 0.5; // 0.0-1.0
const HIHAT_FILTER_FREQ = 8000; // Hz - highpass cutoff
const HIHAT_FILTER_Q = 1.0; // Filter resonance
const HIHAT_MIN_GAIN = 0.01; // Minimum for exponential ramp

// Master synthesizer constants
const MASTER_VOLUME = 0.8; // 0.0-1.0

/**
 * Synthesize a kick drum sound
 * Low-frequency sine wave (50-80Hz) with exponential decay
 * Returns the oscillator for cleanup tracking
 */
function synthKick(
  audioContext: AudioContext,
  destination: AudioNode,
  startTime: number,
  strength: number = 1.0
): OscillatorNode {
  // Oscillator for the "thump" - pitch drops from start to end frequency
  const osc = audioContext.createOscillator();
  const oscGain = audioContext.createGain();

  osc.type = 'sine';
  osc.frequency.setValueAtTime(KICK_START_FREQ, startTime);
  osc.frequency.exponentialRampToValueAtTime(KICK_END_FREQ, startTime + KICK_PITCH_BEND_TIME);

  // Amplitude envelope - quick attack, exponential decay
  oscGain.gain.setValueAtTime(KICK_ATTACK_GAIN * strength, startTime);
  oscGain.gain.exponentialRampToValueAtTime(KICK_MIN_GAIN, startTime + KICK_DECAY_TIME);

  osc.connect(oscGain);
  oscGain.connect(destination);

  osc.start(startTime);
  osc.stop(startTime + KICK_DECAY_TIME);

  return osc;
}

/**
 * Synthesize a snare drum sound
 * Filtered white noise + tonal component for shell resonance
 * Returns noise source and oscillator for cleanup tracking
 */
function synthSnare(
  audioContext: AudioContext,
  destination: AudioNode,
  startTime: number,
  strength: number = 1.0
): [AudioBufferSourceNode, OscillatorNode] {
  // Noise component (rattle)
  const bufferSize = audioContext.sampleRate * SNARE_DURATION;
  const buffer = audioContext.createBuffer(1, bufferSize, audioContext.sampleRate);
  const data = buffer.getChannelData(0);

  // Generate normalized noise (-1 to 1), strength applied via gain node
  for (let i = 0; i < bufferSize; i++) {
    data[i] = Math.random() * 2 - 1;
  }

  const noise = audioContext.createBufferSource();
  noise.buffer = buffer;

  // High-pass filter for noise (gives it "snare" character)
  const noiseFilter = audioContext.createBiquadFilter();
  noiseFilter.type = 'highpass';
  noiseFilter.frequency.value = SNARE_FILTER_FREQ;

  const noiseGain = audioContext.createGain();
  noiseGain.gain.setValueAtTime(SNARE_NOISE_GAIN * strength, startTime);
  noiseGain.gain.exponentialRampToValueAtTime(SNARE_MIN_GAIN, startTime + SNARE_DURATION);

  noise.connect(noiseFilter);
  noiseFilter.connect(noiseGain);
  noiseGain.connect(destination);

  // Tonal component (shell resonance)
  const toneOsc = audioContext.createOscillator();
  const toneGain = audioContext.createGain();

  toneOsc.type = 'triangle';
  toneOsc.frequency.value = SNARE_TONE_FREQ;

  toneGain.gain.setValueAtTime(SNARE_TONE_GAIN * strength, startTime);
  toneGain.gain.exponentialRampToValueAtTime(SNARE_MIN_GAIN, startTime + SNARE_TONE_DECAY);

  toneOsc.connect(toneGain);
  toneGain.connect(destination);

  // Start both components
  noise.start(startTime);
  toneOsc.start(startTime);
  toneOsc.stop(startTime + SNARE_TONE_DECAY);

  return [noise, toneOsc];
}

/**
 * Synthesize a hi-hat sound
 * High-frequency filtered noise with very short decay
 * Returns noise source for cleanup tracking
 */
function synthHihat(
  audioContext: AudioContext,
  destination: AudioNode,
  startTime: number,
  strength: number = 1.0
): AudioBufferSourceNode {
  // White noise
  const bufferSize = audioContext.sampleRate * HIHAT_DURATION;
  const buffer = audioContext.createBuffer(1, bufferSize, audioContext.sampleRate);
  const data = buffer.getChannelData(0);

  // Generate normalized noise (-1 to 1), strength applied via gain node
  for (let i = 0; i < bufferSize; i++) {
    data[i] = Math.random() * 2 - 1;
  }

  const noise = audioContext.createBufferSource();
  noise.buffer = buffer;

  // High-pass filter for metallic character
  const filter = audioContext.createBiquadFilter();
  filter.type = 'highpass';
  filter.frequency.value = HIHAT_FILTER_FREQ;
  filter.Q.value = HIHAT_FILTER_Q;

  const gain = audioContext.createGain();
  gain.gain.setValueAtTime(HIHAT_GAIN * strength, startTime);
  gain.gain.exponentialRampToValueAtTime(HIHAT_MIN_GAIN, startTime + HIHAT_DURATION);

  noise.connect(filter);
  filter.connect(gain);
  gain.connect(destination);

  noise.start(startTime);

  return noise;
}

/**
 * Percussion synthesizer class
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
   * Trigger a percussion sound at specified absolute time
   * @param type - Percussion type (kick/snare/hihat)
   * @param audioTime - Absolute time in AudioContext timeline (seconds)
   * @param strength - Hit strength 0.0-1.0
   */
  trigger(type: PercussionType, audioTime: number, strength: number = 1.0): void {
    switch (type) {
      case 'kick': {
        const osc = synthKick(this.audioContext, this.masterGain, audioTime, strength);
        this.scheduledSources.push(osc);
        break;
      }
      case 'snare': {
        const [noise, osc] = synthSnare(this.audioContext, this.masterGain, audioTime, strength);
        this.scheduledSources.push(noise, osc);
        break;
      }
      case 'hihat': {
        const noise = synthHihat(this.audioContext, this.masterGain, audioTime, strength);
        this.scheduledSources.push(noise);
        break;
      }
    }
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
