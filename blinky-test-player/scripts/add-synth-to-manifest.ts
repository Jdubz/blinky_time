#!/usr/bin/env npx tsx
/**
 * Add synthetically generated samples to the curated manifest
 */

import { readFileSync, writeFileSync, readdirSync, existsSync, statSync } from 'fs';
import { join, basename, dirname } from 'path';
import { fileURLToPath } from 'url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);
const PROJECT_ROOT = join(__dirname, '..');
const SAMPLES_PATH = join(PROJECT_ROOT, 'samples');
const MANIFEST_PATH = join(SAMPLES_PATH, 'manifest.json');

// Synthetic sample types to scan
const SYNTH_TYPES = ['bass', 'synth_stab', 'lead', 'pad', 'chord'];

interface CuratedSample {
  id: string;
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
  byType: Record<string, Record<string, CuratedSample[]>>;
}

// Estimate RMS based on loudness in filename
function estimateRms(id: string): number {
  if (id.includes('hard')) return 0.5;
  if (id.includes('medium')) return 0.25;
  if (id.includes('soft') || id.includes('slow')) return 0.1;
  return 0.3;
}

// Get loudness from filename
function getLoudness(id: string): 'soft' | 'medium' | 'hard' {
  if (id.includes('hard')) return 'hard';
  if (id.includes('medium')) return 'medium';
  if (id.includes('soft') || id.includes('slow')) return 'soft';
  return 'medium';
}

// Estimate duration based on type
function estimateDuration(type: string): number {
  if (type === 'pad') return 2000;
  if (type === 'chord') return 1500;
  if (type === 'bass') return 500;
  if (type === 'synth_stab') return 300;
  if (type === 'lead') return 400;
  return 500;
}

async function main() {
  console.log('Adding synthetic samples to manifest...\n');

  // Read existing manifest
  let manifest: CuratedManifest;
  if (existsSync(MANIFEST_PATH)) {
    manifest = JSON.parse(readFileSync(MANIFEST_PATH, 'utf-8'));
    console.log(`Existing manifest has ${manifest.samples.length} samples`);
  } else {
    manifest = {
      generatedAt: new Date().toISOString(),
      samples: [],
      byType: {},
    };
  }

  // Track what we add
  let added = 0;
  const existingIds = new Set(manifest.samples.map(s => s.id));

  // Scan each synth type folder
  for (const type of SYNTH_TYPES) {
    const typePath = join(SAMPLES_PATH, type);
    if (!existsSync(typePath)) {
      console.log(`  ${type}: folder not found, skipping`);
      continue;
    }

    const files = readdirSync(typePath).filter(f => f.endsWith('.wav'));
    console.log(`  ${type}: found ${files.length} samples`);

    if (!manifest.byType[type]) {
      manifest.byType[type] = {};
    }

    for (const file of files) {
      const id = file.replace('.wav', '');

      // Skip if already in manifest
      if (existingIds.has(id)) {
        console.log(`    ${id}: already in manifest`);
        continue;
      }

      const loudness = getLoudness(id);
      const sample: CuratedSample = {
        id,
        originalPath: join(typePath, file),
        newPath: `samples\\${type}\\${file}`,
        type,
        loudness,
        rms: estimateRms(id),
        peak: 0.9,
        durationMs: estimateDuration(type),
      };

      manifest.samples.push(sample);
      existingIds.add(id);

      if (!manifest.byType[type][loudness]) {
        manifest.byType[type][loudness] = [];
      }
      manifest.byType[type][loudness].push(sample);

      console.log(`    Added: ${id} (${loudness})`);
      added++;
    }
  }

  // Update timestamp
  manifest.generatedAt = new Date().toISOString();

  // Write updated manifest
  writeFileSync(MANIFEST_PATH, JSON.stringify(manifest, null, 2));

  console.log(`\nAdded ${added} new samples`);
  console.log(`Total samples in manifest: ${manifest.samples.length}`);
  console.log(`Manifest updated: ${MANIFEST_PATH}`);
}

main().catch(console.error);
