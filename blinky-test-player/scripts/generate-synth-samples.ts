#!/usr/bin/env npx tsx
/**
 * Generate synthetic audio samples for transient detection testing
 *
 * Creates bass, synth stab, lead, pad, and chord samples with varying
 * attack characteristics to test onset detection algorithms.
 *
 * Run: npx tsx scripts/generate-synth-samples.ts
 */

import { writeFileSync, mkdirSync, existsSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);
const PROJECT_ROOT = join(__dirname, '..');
const SAMPLES_PATH = join(PROJECT_ROOT, 'samples');

// Audio parameters
const SAMPLE_RATE = 44100;
const BIT_DEPTH = 16;

/**
 * Generate a WAV file header
 */
function createWavHeader(dataLength: number): Buffer {
  const header = Buffer.alloc(44);

  // RIFF header
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + dataLength, 4);
  header.write('WAVE', 8);

  // fmt chunk
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16); // chunk size
  header.writeUInt16LE(1, 20); // audio format (PCM)
  header.writeUInt16LE(1, 22); // num channels (mono)
  header.writeUInt32LE(SAMPLE_RATE, 24);
  header.writeUInt32LE(SAMPLE_RATE * 2, 28); // byte rate
  header.writeUInt16LE(2, 32); // block align
  header.writeUInt16LE(BIT_DEPTH, 34);

  // data chunk
  header.write('data', 36);
  header.writeUInt32LE(dataLength, 40);

  return header;
}

/**
 * Convert float samples (-1 to 1) to 16-bit PCM buffer
 */
function floatToPcm(samples: number[]): Buffer {
  const buffer = Buffer.alloc(samples.length * 2);
  for (let i = 0; i < samples.length; i++) {
    const sample = Math.max(-1, Math.min(1, samples[i]));
    const value = Math.round(sample * 32767);
    buffer.writeInt16LE(value, i * 2);
  }
  return buffer;
}

/**
 * Save samples as WAV file
 */
function saveWav(filename: string, samples: number[]): void {
  const pcmData = floatToPcm(samples);
  const header = createWavHeader(pcmData.length);
  const wav = Buffer.concat([header, pcmData]);

  const dir = dirname(filename);
  if (!existsSync(dir)) {
    mkdirSync(dir, { recursive: true });
  }

  writeFileSync(filename, wav);
  console.log(`  Created: ${filename}`);
}

/**
 * Envelope generators
 */
function adsrEnvelope(
  t: number,
  attack: number,
  decay: number,
  sustain: number,
  release: number,
  duration: number
): number {
  if (t < attack) {
    return t / attack; // Attack phase
  } else if (t < attack + decay) {
    const decayT = (t - attack) / decay;
    return 1 - decayT * (1 - sustain); // Decay phase
  } else if (t < duration - release) {
    return sustain; // Sustain phase
  } else if (t < duration) {
    const releaseT = (t - (duration - release)) / release;
    return sustain * (1 - releaseT); // Release phase
  }
  return 0;
}

/**
 * Oscillators
 */
function sine(phase: number): number {
  return Math.sin(phase * 2 * Math.PI);
}

function saw(phase: number): number {
  return 2 * (phase - Math.floor(phase + 0.5));
}

function square(phase: number): number {
  return phase % 1 < 0.5 ? 1 : -1;
}

/**
 * Simple lowpass filter (one-pole)
 */
function lowpass(samples: number[], cutoff: number): number[] {
  const rc = 1 / (2 * Math.PI * cutoff);
  const dt = 1 / SAMPLE_RATE;
  const alpha = dt / (rc + dt);

  const output: number[] = new Array(samples.length);
  output[0] = samples[0];

  for (let i = 1; i < samples.length; i++) {
    output[i] = output[i - 1] + alpha * (samples[i] - output[i - 1]);
  }

  return output;
}

/**
 * Generate bass samples
 * - Low frequency (60-100 Hz)
 * - Sharp attack (transient)
 * - Short decay
 */
function generateBass(): void {
  console.log('\nGenerating bass samples...');
  const dir = join(SAMPLES_PATH, 'bass');

  const configs = [
    { name: 'bass_hard_1', freq: 60, attack: 0.005, level: 0.9 },
    { name: 'bass_hard_2', freq: 80, attack: 0.008, level: 0.85 },
    { name: 'bass_medium_1', freq: 70, attack: 0.015, level: 0.6 },
    { name: 'bass_medium_2', freq: 90, attack: 0.020, level: 0.55 },
    { name: 'bass_soft_1', freq: 65, attack: 0.030, level: 0.35 },
  ];

  for (const cfg of configs) {
    const duration = 0.5; // 500ms
    const numSamples = Math.floor(duration * SAMPLE_RATE);
    const samples: number[] = new Array(numSamples);

    for (let i = 0; i < numSamples; i++) {
      const t = i / SAMPLE_RATE;
      const phase = t * cfg.freq;
      const env = adsrEnvelope(t, cfg.attack, 0.1, 0.4, 0.15, duration);
      samples[i] = sine(phase) * env * cfg.level;
    }

    // Add some harmonics for punch
    const filtered = lowpass(samples, 200);
    saveWav(join(dir, `${cfg.name}.wav`), filtered);
  }
}

/**
 * Generate synth stab samples
 * - Sharp, punchy attack
 * - High frequency content
 * - Short duration
 */
function generateSynthStabs(): void {
  console.log('\nGenerating synth stab samples...');
  const dir = join(SAMPLES_PATH, 'synth_stab');

  const configs = [
    { name: 'synth_stab_hard_1', freq: 220, attack: 0.002, level: 0.85 },
    { name: 'synth_stab_hard_2', freq: 330, attack: 0.003, level: 0.8 },
    { name: 'synth_stab_medium_1', freq: 261, attack: 0.010, level: 0.55 },
    { name: 'synth_stab_soft_1', freq: 196, attack: 0.020, level: 0.35 },
  ];

  for (const cfg of configs) {
    const duration = 0.3; // 300ms
    const numSamples = Math.floor(duration * SAMPLE_RATE);
    const samples: number[] = new Array(numSamples);

    for (let i = 0; i < numSamples; i++) {
      const t = i / SAMPLE_RATE;
      const phase = t * cfg.freq;
      const env = adsrEnvelope(t, cfg.attack, 0.05, 0.2, 0.1, duration);

      // Sawtooth with filter sweep
      const filterEnv = 500 + 2000 * Math.exp(-t * 10);
      let sample = saw(phase) * 0.7 + saw(phase * 2) * 0.2 + saw(phase * 3) * 0.1;
      sample *= env * cfg.level;
      samples[i] = sample;
    }

    const filtered = lowpass(samples, 3000);
    saveWav(join(dir, `${cfg.name}.wav`), filtered);
  }
}

/**
 * Generate lead samples
 * - Medium attack (noticeable transient)
 * - Melodic frequencies
 */
function generateLead(): void {
  console.log('\nGenerating lead samples...');
  const dir = join(SAMPLES_PATH, 'lead');

  const configs = [
    { name: 'lead_hard_1', freq: 440, attack: 0.005, level: 0.75 },
    { name: 'lead_hard_2', freq: 523, attack: 0.008, level: 0.7 },
    { name: 'lead_medium_1', freq: 392, attack: 0.015, level: 0.5 },
    { name: 'lead_soft_1', freq: 330, attack: 0.030, level: 0.3 },
  ];

  for (const cfg of configs) {
    const duration = 0.4; // 400ms
    const numSamples = Math.floor(duration * SAMPLE_RATE);
    const samples: number[] = new Array(numSamples);

    for (let i = 0; i < numSamples; i++) {
      const t = i / SAMPLE_RATE;
      const phase = t * cfg.freq;
      const env = adsrEnvelope(t, cfg.attack, 0.08, 0.6, 0.15, duration);

      // Square wave with some filtering
      let sample = square(phase) * 0.8 + sine(phase * 2) * 0.2;
      sample *= env * cfg.level;
      samples[i] = sample;
    }

    const filtered = lowpass(samples, 4000);
    saveWav(join(dir, `${cfg.name}.wav`), filtered);
  }
}

/**
 * Generate pad samples
 * - SLOW attack (should NOT trigger transient detection!)
 * - Sustained
 * - This tests false positive rejection
 */
function generatePads(): void {
  console.log('\nGenerating pad samples (slow attack - should NOT trigger)...');
  const dir = join(SAMPLES_PATH, 'pad');

  const configs = [
    { name: 'pad_slow_1', freq: 130, attack: 0.5, level: 0.7 },   // Very slow attack
    { name: 'pad_slow_2', freq: 196, attack: 0.8, level: 0.65 },  // Extra slow
    { name: 'pad_medium_1', freq: 165, attack: 0.3, level: 0.5 }, // Medium-slow
  ];

  for (const cfg of configs) {
    const duration = 2.0; // 2 seconds (long sustained sound)
    const numSamples = Math.floor(duration * SAMPLE_RATE);
    const samples: number[] = new Array(numSamples);

    for (let i = 0; i < numSamples; i++) {
      const t = i / SAMPLE_RATE;
      const phase = t * cfg.freq;
      const env = adsrEnvelope(t, cfg.attack, 0.2, 0.8, 0.5, duration);

      // Soft pad sound - multiple detuned sines
      let sample = sine(phase) * 0.5;
      sample += sine(phase * 1.003) * 0.3; // Slight detune for richness
      sample += sine(phase * 0.997) * 0.2;
      sample *= env * cfg.level;
      samples[i] = sample;
    }

    const filtered = lowpass(samples, 800);
    saveWav(join(dir, `${cfg.name}.wav`), filtered);
  }
}

/**
 * Generate chord samples
 * - Slow attack (should NOT trigger)
 * - Multiple notes
 */
function generateChords(): void {
  console.log('\nGenerating chord samples (slow attack - should NOT trigger)...');
  const dir = join(SAMPLES_PATH, 'chord');

  // Major chord frequencies (root, third, fifth)
  const chords = [
    { name: 'chord_slow_1', freqs: [130, 164, 196], attack: 0.4, level: 0.6 }, // C major
    { name: 'chord_slow_2', freqs: [146, 185, 220], attack: 0.5, level: 0.55 }, // D major
    { name: 'chord_medium_1', freqs: [164, 207, 247], attack: 0.2, level: 0.5 }, // E major (faster but still slow)
  ];

  for (const cfg of chords) {
    const duration = 1.5; // 1.5 seconds
    const numSamples = Math.floor(duration * SAMPLE_RATE);
    const samples: number[] = new Array(numSamples);

    for (let i = 0; i < numSamples; i++) {
      const t = i / SAMPLE_RATE;
      const env = adsrEnvelope(t, cfg.attack, 0.15, 0.7, 0.4, duration);

      let sample = 0;
      for (const freq of cfg.freqs) {
        const phase = t * freq;
        sample += sine(phase) / cfg.freqs.length;
      }
      sample *= env * cfg.level;
      samples[i] = sample;
    }

    const filtered = lowpass(samples, 1500);
    saveWav(join(dir, `${cfg.name}.wav`), filtered);
  }
}

// Main execution
console.log('=== Generating Synthetic Samples for Transient Detection Testing ===');
console.log(`Output directory: ${SAMPLES_PATH}`);

generateBass();
generateSynthStabs();
generateLead();
generatePads();
generateChords();

console.log('\n=== Sample Generation Complete ===');
console.log('Transient samples (should trigger): bass, synth_stab, lead');
console.log('Sustained samples (should NOT trigger): pad, chord');
console.log('\nNote: Run "npm run build" to rebuild before testing.');
