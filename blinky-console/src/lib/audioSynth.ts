/**
 * Web Audio API percussion synthesizer
 *
 * Generates kick, snare, and hi-hat sounds programmatically
 * without requiring audio file samples.
 */

import type { PercussionType } from '../types/testTypes';

/**
 * Synthesize a kick drum sound
 * Low-frequency sine wave (50-80Hz) with exponential decay
 */
function synthKick(
  audioContext: AudioContext,
  destination: AudioNode,
  startTime: number,
  strength: number = 1.0
): void {
  // Oscillator for the "thump" - pitch drops from 150Hz to 50Hz
  const osc = audioContext.createOscillator();
  const oscGain = audioContext.createGain();

  osc.type = 'sine';
  osc.frequency.setValueAtTime(150, startTime);
  osc.frequency.exponentialRampToValueAtTime(50, startTime + 0.05);

  // Amplitude envelope - quick attack, exponential decay
  oscGain.gain.setValueAtTime(0.8 * strength, startTime);
  oscGain.gain.exponentialRampToValueAtTime(0.01, startTime + 0.5);

  osc.connect(oscGain);
  oscGain.connect(destination);

  osc.start(startTime);
  osc.stop(startTime + 0.5);
}

/**
 * Synthesize a snare drum sound
 * Filtered white noise + tonal component for shell resonance
 */
function synthSnare(
  audioContext: AudioContext,
  destination: AudioNode,
  startTime: number,
  strength: number = 1.0
): void {
  const duration = 0.2;

  // Noise component (rattle)
  const bufferSize = audioContext.sampleRate * duration;
  const buffer = audioContext.createBuffer(1, bufferSize, audioContext.sampleRate);
  const data = buffer.getChannelData(0);

  for (let i = 0; i < bufferSize; i++) {
    data[i] = (Math.random() * 2 - 1) * strength;
  }

  const noise = audioContext.createBufferSource();
  noise.buffer = buffer;

  // High-pass filter for noise (gives it "snare" character)
  const noiseFilter = audioContext.createBiquadFilter();
  noiseFilter.type = 'highpass';
  noiseFilter.frequency.value = 1000;

  const noiseGain = audioContext.createGain();
  noiseGain.gain.setValueAtTime(0.7 * strength, startTime);
  noiseGain.gain.exponentialRampToValueAtTime(0.01, startTime + duration);

  noise.connect(noiseFilter);
  noiseFilter.connect(noiseGain);
  noiseGain.connect(destination);

  // Tonal component (shell resonance)
  const toneOsc = audioContext.createOscillator();
  const toneGain = audioContext.createGain();

  toneOsc.type = 'triangle';
  toneOsc.frequency.value = 180;

  toneGain.gain.setValueAtTime(0.3 * strength, startTime);
  toneGain.gain.exponentialRampToValueAtTime(0.01, startTime + 0.1);

  toneOsc.connect(toneGain);
  toneGain.connect(destination);

  // Start both components
  noise.start(startTime);
  toneOsc.start(startTime);
  toneOsc.stop(startTime + 0.1);
}

/**
 * Synthesize a hi-hat sound
 * High-frequency filtered noise with very short decay
 */
function synthHihat(
  audioContext: AudioContext,
  destination: AudioNode,
  startTime: number,
  strength: number = 1.0
): void {
  const duration = 0.05;

  // White noise
  const bufferSize = audioContext.sampleRate * duration;
  const buffer = audioContext.createBuffer(1, bufferSize, audioContext.sampleRate);
  const data = buffer.getChannelData(0);

  for (let i = 0; i < bufferSize; i++) {
    data[i] = (Math.random() * 2 - 1) * strength;
  }

  const noise = audioContext.createBufferSource();
  noise.buffer = buffer;

  // High-pass filter (8kHz+) for metallic character
  const filter = audioContext.createBiquadFilter();
  filter.type = 'highpass';
  filter.frequency.value = 8000;
  filter.Q.value = 1.0;

  const gain = audioContext.createGain();
  gain.gain.setValueAtTime(0.5 * strength, startTime);
  gain.gain.exponentialRampToValueAtTime(0.01, startTime + duration);

  noise.connect(filter);
  filter.connect(gain);
  gain.connect(destination);

  noise.start(startTime);
}

/**
 * Percussion synthesizer class
 * Manages AudioContext and provides simple interface for triggering sounds
 */
export class PercussionSynth {
  private audioContext: AudioContext;
  private masterGain: GainNode;

  constructor() {
    this.audioContext = new AudioContext();
    this.masterGain = this.audioContext.createGain();
    this.masterGain.gain.value = 0.8; // Master volume
    this.masterGain.connect(this.audioContext.destination);
  }

  /**
   * Trigger a percussion sound at specified time
   * @param type - Percussion type (kick/snare/hihat)
   * @param timeSeconds - When to play (in seconds relative to AudioContext time)
   * @param strength - Hit strength 0.0-1.0
   */
  trigger(type: PercussionType, timeSeconds: number, strength: number = 1.0): void {
    const audioTime = this.audioContext.currentTime + timeSeconds;

    switch (type) {
      case 'kick':
        synthKick(this.audioContext, this.masterGain, audioTime, strength);
        break;
      case 'snare':
        synthSnare(this.audioContext, this.masterGain, audioTime, strength);
        break;
      case 'hihat':
        synthHihat(this.audioContext, this.masterGain, audioTime, strength);
        break;
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
   * Stop all audio playback immediately by muting master gain
   */
  stop(): void {
    // Immediately cut off all audio
    this.masterGain.gain.setValueAtTime(0, this.audioContext.currentTime);
  }

  /**
   * Resume audio playback by restoring master gain
   */
  start(): void {
    // Restore master volume
    this.masterGain.gain.setValueAtTime(0.8, this.audioContext.currentTime);
  }

  /**
   * Clean up resources
   */
  dispose(): void {
    this.audioContext.close();
  }
}
