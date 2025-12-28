#!/usr/bin/env npx tsx
/**
 * Curate a deterministic sample set from the analyzed manifest
 * Selects representative samples from each loudness category
 */

import { readFileSync, writeFileSync, copyFileSync, mkdirSync, existsSync, rmSync } from 'fs';
import { join, basename } from 'path';

interface SampleMetadata {
  path: string;
  name: string;
  type: string;
  rms: number;
  peak: number;
  durationMs: number;
  loudnessCategory: 'soft' | 'medium' | 'hard';
}

interface SampleManifest {
  samples: Record<string, SampleMetadata[]>;
}

interface CuratedSample {
  id: string;           // e.g., "kick_hard_1"
  originalPath: string;
  newPath: string;
  type: string;
  loudness: 'soft' | 'medium' | 'hard';
  rms: number;
  peak: number;
  durationMs: number;
}

interface CuratedManifest {
  generatedAt: string;
  samples: CuratedSample[];
  byType: Record<string, Record<string, CuratedSample[]>>; // type -> loudness -> samples
}

// Selection criteria: pick samples that are well-separated in loudness
const SELECTION_CONFIG = {
  kick: {
    hard: { minRms: 0.35, maxRms: 0.70, count: 3 },
    medium: { minRms: 0.15, maxRms: 0.35, count: 3 },
    soft: { minRms: 0.05, maxRms: 0.15, count: 3 },
  },
  snare: {
    hard: { minRms: 0.25, maxRms: 0.50, count: 3 },
    medium: { minRms: 0.10, maxRms: 0.25, count: 3 },
    soft: { minRms: 0.02, maxRms: 0.10, count: 3 },
  },
  hat: {
    hard: { minRms: 0.15, maxRms: 0.40, count: 3 },
    medium: { minRms: 0.05, maxRms: 0.15, count: 3 },
    soft: { minRms: 0.01, maxRms: 0.05, count: 3 },
  },
  tom: {
    hard: { minRms: 0.20, maxRms: 0.50, count: 2 },
    medium: { minRms: 0.10, maxRms: 0.20, count: 2 },
    soft: { minRms: 0.03, maxRms: 0.10, count: 1 },
  },
  clap: {
    hard: { minRms: 0.15, maxRms: 0.40, count: 2 },
    medium: { minRms: 0.05, maxRms: 0.15, count: 2 },
    soft: { minRms: 0.02, maxRms: 0.05, count: 1 },
  },
};

function selectSamples(
  samples: SampleMetadata[],
  minRms: number,
  maxRms: number,
  count: number
): SampleMetadata[] {
  // Filter to range
  const inRange = samples.filter(s => s.rms >= minRms && s.rms <= maxRms);

  if (inRange.length === 0) {
    console.warn(`  Warning: No samples in range ${minRms}-${maxRms}`);
    return [];
  }

  // Sort by RMS and pick evenly spaced samples
  inRange.sort((a, b) => b.rms - a.rms);

  const selected: SampleMetadata[] = [];
  const step = Math.max(1, Math.floor(inRange.length / count));

  for (let i = 0; i < count && i * step < inRange.length; i++) {
    selected.push(inRange[i * step]);
  }

  return selected;
}

async function main() {
  const manifestPath = process.argv[2] || 'sample-manifest.json';
  const outputDir = process.argv[3] || 'samples';

  console.log(`Reading manifest from: ${manifestPath}`);
  const manifest: SampleManifest = JSON.parse(readFileSync(manifestPath, 'utf-8'));

  // Clear and recreate output directory
  if (existsSync(outputDir)) {
    console.log(`Clearing existing samples in: ${outputDir}`);
    rmSync(outputDir, { recursive: true });
  }
  mkdirSync(outputDir, { recursive: true });

  const curated: CuratedSample[] = [];
  const byType: Record<string, Record<string, CuratedSample[]>> = {};

  // Process each type
  for (const [type, config] of Object.entries(SELECTION_CONFIG)) {
    const samples = manifest.samples[type] || [];
    console.log(`\nProcessing ${type}: ${samples.length} available`);

    if (!byType[type]) byType[type] = {};
    const typeDir = join(outputDir, type);
    mkdirSync(typeDir, { recursive: true });

    for (const [loudness, criteria] of Object.entries(config)) {
      const { minRms, maxRms, count } = criteria as { minRms: number; maxRms: number; count: number };
      const selected = selectSamples(samples, minRms, maxRms, count);

      console.log(`  ${loudness}: selected ${selected.length}/${count} (range ${minRms}-${maxRms})`);

      if (!byType[type][loudness]) byType[type][loudness] = [];

      for (let i = 0; i < selected.length; i++) {
        const sample = selected[i];
        const id = `${type}_${loudness}_${i + 1}`;
        const newFilename = `${id}.wav`;
        const newPath = join(typeDir, newFilename);

        // Copy file
        copyFileSync(sample.path, newPath);

        const curatedSample: CuratedSample = {
          id,
          originalPath: sample.path,
          newPath,
          type,
          loudness: loudness as 'soft' | 'medium' | 'hard',
          rms: sample.rms,
          peak: sample.peak,
          durationMs: sample.durationMs,
        };

        curated.push(curatedSample);
        byType[type][loudness].push(curatedSample);

        console.log(`    ${id}: ${sample.name} (RMS: ${sample.rms.toFixed(3)})`);
      }
    }
  }

  // Write curated manifest
  const curatedManifest: CuratedManifest = {
    generatedAt: new Date().toISOString(),
    samples: curated,
    byType,
  };

  const manifestOutputPath = join(outputDir, 'manifest.json');
  writeFileSync(manifestOutputPath, JSON.stringify(curatedManifest, null, 2));

  console.log(`\n=== Summary ===`);
  console.log(`Total curated samples: ${curated.length}`);
  for (const [type, loudnesses] of Object.entries(byType)) {
    const total = Object.values(loudnesses).flat().length;
    console.log(`  ${type}: ${total}`);
  }
  console.log(`\nManifest written to: ${manifestOutputPath}`);
}

main().catch(console.error);
