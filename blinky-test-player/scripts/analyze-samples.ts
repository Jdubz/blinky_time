#!/usr/bin/env npx ts-node
/**
 * Analyze drum samples for loudness and frequency characteristics
 * Outputs a manifest.json with metadata for deterministic test selection
 */

import { readFileSync, writeFileSync, readdirSync, statSync, existsSync } from 'fs';
import { join, basename, extname } from 'path';

interface SampleMetadata {
  path: string;
  name: string;
  type: string;
  rms: number;        // RMS amplitude (0-1)
  peak: number;       // Peak amplitude (0-1)
  durationMs: number; // Duration in milliseconds
  loudnessCategory: 'soft' | 'medium' | 'hard';
}

interface SampleManifest {
  analyzedAt: string;
  sampleCount: number;
  samples: Record<string, SampleMetadata[]>; // Grouped by type (kick, snare, etc.)
}

/**
 * Parse WAV file header and extract audio data
 */
function parseWav(buffer: Buffer): { sampleRate: number; samples: Float32Array } | null {
  // Check RIFF header
  if (buffer.toString('ascii', 0, 4) !== 'RIFF') return null;
  if (buffer.toString('ascii', 8, 12) !== 'WAVE') return null;

  let pos = 12;
  let sampleRate = 44100;
  let bitsPerSample = 16;
  let numChannels = 1;
  let dataStart = 0;
  let dataSize = 0;

  // Parse chunks
  while (pos < buffer.length - 8) {
    const chunkId = buffer.toString('ascii', pos, pos + 4);
    const chunkSize = buffer.readUInt32LE(pos + 4);

    if (chunkId === 'fmt ') {
      const audioFormat = buffer.readUInt16LE(pos + 8);
      if (audioFormat !== 1 && audioFormat !== 3) {
        // Only support PCM (1) and IEEE float (3)
        return null;
      }
      numChannels = buffer.readUInt16LE(pos + 10);
      sampleRate = buffer.readUInt32LE(pos + 12);
      bitsPerSample = buffer.readUInt16LE(pos + 22);
    } else if (chunkId === 'data') {
      dataStart = pos + 8;
      dataSize = chunkSize;
      break;
    }

    pos += 8 + chunkSize;
    if (chunkSize % 2 !== 0) pos++; // Padding byte
  }

  if (dataStart === 0) return null;

  // Convert to float samples (mono, normalized to -1..1)
  const bytesPerSample = bitsPerSample / 8;
  const numSamples = Math.floor(dataSize / bytesPerSample / numChannels);
  const samples = new Float32Array(numSamples);

  for (let i = 0; i < numSamples; i++) {
    let sample = 0;
    const offset = dataStart + i * bytesPerSample * numChannels;

    if (bitsPerSample === 16) {
      // 16-bit signed PCM
      sample = buffer.readInt16LE(offset) / 32768;
    } else if (bitsPerSample === 24) {
      // 24-bit signed PCM
      const b0 = buffer[offset];
      const b1 = buffer[offset + 1];
      const b2 = buffer[offset + 2];
      const value = (b2 << 16) | (b1 << 8) | b0;
      sample = (value > 0x7FFFFF ? value - 0x1000000 : value) / 8388608;
    } else if (bitsPerSample === 32) {
      // 32-bit float
      sample = buffer.readFloatLE(offset);
    }

    // If stereo, just use first channel
    samples[i] = sample;
  }

  return { sampleRate, samples };
}

/**
 * Calculate RMS (root mean square) amplitude
 */
function calculateRms(samples: Float32Array): number {
  let sum = 0;
  for (let i = 0; i < samples.length; i++) {
    sum += samples[i] * samples[i];
  }
  return Math.sqrt(sum / samples.length);
}

/**
 * Calculate peak amplitude
 */
function calculatePeak(samples: Float32Array): number {
  let peak = 0;
  for (let i = 0; i < samples.length; i++) {
    const abs = Math.abs(samples[i]);
    if (abs > peak) peak = abs;
  }
  return peak;
}

/**
 * Categorize loudness based on RMS
 */
function categorizeLoudness(rms: number): 'soft' | 'medium' | 'hard' {
  // These thresholds are tuned for typical drum samples
  // RMS of 0.1 is quiet, 0.2 is medium, 0.3+ is loud
  if (rms < 0.12) return 'soft';
  if (rms < 0.22) return 'medium';
  return 'hard';
}

/**
 * Determine sample type from filename
 */
function detectType(filename: string): string {
  const lower = filename.toLowerCase();
  if (lower.includes('kick')) return 'kick';
  if (lower.includes('snare')) return 'snare';
  if (lower.includes('hihat') || lower.includes('hi-hat') || lower.includes('hat')) return 'hat';
  if (lower.includes('tom')) return 'tom';
  if (lower.includes('clap')) return 'clap';
  if (lower.includes('perc')) return 'percussion';
  if (lower.includes('bass')) return 'bass';
  return 'unknown';
}

/**
 * Recursively find all WAV files
 */
function findWavFiles(dir: string): string[] {
  const files: string[] = [];

  function walk(currentDir: string) {
    try {
      const entries = readdirSync(currentDir);
      for (const entry of entries) {
        const fullPath = join(currentDir, entry);
        const stat = statSync(fullPath);
        if (stat.isDirectory()) {
          walk(fullPath);
        } else if (extname(entry).toLowerCase() === '.wav') {
          files.push(fullPath);
        }
      }
    } catch (err) {
      // Skip inaccessible directories
    }
  }

  walk(dir);
  return files;
}

/**
 * Analyze a single sample file
 */
function analyzeSample(filePath: string): SampleMetadata | null {
  try {
    const buffer = readFileSync(filePath);
    const parsed = parseWav(buffer);
    if (!parsed) {
      console.error(`  Skipping ${basename(filePath)}: unsupported format`);
      return null;
    }

    const { sampleRate, samples } = parsed;
    const rms = calculateRms(samples);
    const peak = calculatePeak(samples);
    const durationMs = Math.round((samples.length / sampleRate) * 1000);
    const type = detectType(basename(filePath));

    return {
      path: filePath,
      name: basename(filePath),
      type,
      rms: Math.round(rms * 1000) / 1000,
      peak: Math.round(peak * 1000) / 1000,
      durationMs,
      loudnessCategory: categorizeLoudness(rms),
    };
  } catch (err) {
    console.error(`  Error analyzing ${filePath}:`, err);
    return null;
  }
}

// Main
async function main() {
  const args = process.argv.slice(2);
  const sourceDir = args[0] || 'D:/Ableton/drums';
  const outputFile = args[1] || 'sample-manifest.json';

  console.log(`Analyzing samples in: ${sourceDir}`);

  if (!existsSync(sourceDir)) {
    console.error(`Source directory not found: ${sourceDir}`);
    process.exit(1);
  }

  const wavFiles = findWavFiles(sourceDir);
  console.log(`Found ${wavFiles.length} WAV files`);

  const samples: Record<string, SampleMetadata[]> = {};
  let analyzed = 0;

  for (const file of wavFiles) {
    const metadata = analyzeSample(file);
    if (metadata) {
      if (!samples[metadata.type]) {
        samples[metadata.type] = [];
      }
      samples[metadata.type].push(metadata);
      analyzed++;

      if (analyzed % 100 === 0) {
        console.log(`  Analyzed ${analyzed}/${wavFiles.length}...`);
      }
    }
  }

  // Sort each type by RMS (loudest first)
  for (const type of Object.keys(samples)) {
    samples[type].sort((a, b) => b.rms - a.rms);
  }

  const manifest: SampleManifest = {
    analyzedAt: new Date().toISOString(),
    sampleCount: analyzed,
    samples,
  };

  // Print summary
  console.log('\nSummary by type:');
  for (const [type, list] of Object.entries(samples)) {
    const soft = list.filter(s => s.loudnessCategory === 'soft').length;
    const medium = list.filter(s => s.loudnessCategory === 'medium').length;
    const hard = list.filter(s => s.loudnessCategory === 'hard').length;
    console.log(`  ${type}: ${list.length} (soft: ${soft}, medium: ${medium}, hard: ${hard})`);
  }

  writeFileSync(outputFile, JSON.stringify(manifest, null, 2));
  console.log(`\nManifest written to: ${outputFile}`);
}

main().catch(console.error);
