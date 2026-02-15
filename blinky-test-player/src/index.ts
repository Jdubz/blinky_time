#!/usr/bin/env node

/**
 * Blinky Test Pattern Player CLI
 *
 * Plays known test patterns through system audio using Playwright + Tone.js.
 * Uses real drum samples for authentic transient testing.
 * Outputs ground truth timing data for comparison with device detections.
 */

import { chromium } from 'playwright';
import { Command } from 'commander';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { writeFileSync, readFileSync, readdirSync, existsSync } from 'fs';
import { TEST_PATTERNS, getPatternById } from './patterns.js';
import type { PatternOutput, SampleManifest, InstrumentType } from './types.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Project root (one level up from dist/)
const PROJECT_ROOT = join(__dirname, '..');

// Sample folder types (all instrument types that can have audio samples)
const SAMPLE_TYPES: InstrumentType[] = [
  'kick', 'snare', 'hat', 'tom', 'clap', 'percussion', 'bass',  // Drums
  'synth_stab', 'lead', 'pad', 'chord',  // Melodic/harmonic
];

/**
 * Convert a Windows path to a proper file:// URL
 */
function pathToFileUrl(filePath: string): string {
  // Normalize to forward slashes
  const normalized = filePath.replace(/\\/g, '/');
  // Ensure it starts with file:///
  if (normalized.startsWith('/')) {
    return `file://${normalized}`;
  }
  // Windows absolute path (C:/...)
  return `file:///${normalized}`;
}

/**
 * Sample with name and URL
 */
interface SampleInfo {
  name: string;
  url: string;
}

/**
 * Extended manifest with URLs
 */
interface SampleManifestWithUrls {
  [key: string]: SampleInfo[] | undefined;
}

/**
 * Scan samples folder and build manifest of available samples
 */
function scanSamples(samplesPath: string): SampleManifest {
  const manifest: SampleManifest = {};

  for (const type of SAMPLE_TYPES) {
    const folderPath = join(samplesPath, type);
    if (existsSync(folderPath)) {
      try {
        const files = readdirSync(folderPath)
          .filter(f => /\.(wav|mp3|ogg|flac)$/i.test(f));
        if (files.length > 0) {
          manifest[type] = files;
        }
      } catch {
        // Ignore read errors
      }
    }
  }

  return manifest;
}

/**
 * Build manifest with full file:// URLs for each sample
 */
function buildManifestWithUrls(samplesPath: string, manifest: SampleManifest): SampleManifestWithUrls {
  const result: SampleManifestWithUrls = {};

  for (const [type, files] of Object.entries(manifest)) {
    if (!files || files.length === 0) continue;

    result[type] = files.map((file: string) => ({
      name: file,
      url: pathToFileUrl(join(samplesPath, type, file)),
    }));
  }

  return result;
}

/**
 * Get sample counts summary
 */
function getSampleSummary(manifest: SampleManifest): string {
  const parts: string[] = [];
  for (const [type, files] of Object.entries(manifest)) {
    if (files && files.length > 0) {
      parts.push(`${type}: ${files.length}`);
    }
  }
  return parts.length > 0 ? parts.join(', ') : 'No samples found';
}

const program = new Command();

program
  .name('blinky-test-player')
  .description('Play transient test patterns for blinky device testing')
  .version('1.0.0');

program
  .command('list')
  .description('List all available test patterns')
  .action(() => {
    console.log('\nAvailable test patterns:\n');
    for (const pattern of TEST_PATTERNS) {
      console.log(`  ${pattern.id.padEnd(16)} ${pattern.name}`);
      console.log(`  ${''.padEnd(16)} ${pattern.description}`);
      console.log(`  ${''.padEnd(16)} Duration: ${pattern.durationMs / 1000}s, Hits: ${pattern.hits.length}`);
      console.log();
    }
  });

program
  .command('samples')
  .description('List available samples in the samples folder')
  .action(() => {
    const samplesPath = join(PROJECT_ROOT, 'samples');
    const manifest = scanSamples(samplesPath);

    console.log('\nSample folders:\n');
    for (const type of SAMPLE_TYPES) {
      const files = manifest[type] || [];
      console.log(`  ${type.padEnd(12)} ${files.length} sample(s)`);
      for (const file of files.slice(0, 5)) {
        console.log(`    - ${file}`);
      }
      if (files.length > 5) {
        console.log(`    ... and ${files.length - 5} more`);
      }
    }

    console.log(`\nSamples path: ${samplesPath}`);
    console.log('Add .wav/.mp3/.ogg/.flac files to the appropriate folders.');
  });

program
  .command('play <pattern>')
  .description('Play a test pattern using samples')
  .option('-o, --output <file>', 'Output ground truth to file (default: stdout)')
  .option('-f, --format <fmt>', 'Output format: json | csv', 'json')
  .option('-d, --delay <ms>', 'Delay before starting in milliseconds', '500')
  .option('-q, --quiet', 'Suppress progress messages')
  .option('--headless', 'Run browser in headless mode (no visible window)')
  .option('-s, --samples <path>', 'Path to samples folder', join(PROJECT_ROOT, 'samples'))
  .action(async (patternId: string, options) => {
    const pattern = getPatternById(patternId);

    if (!pattern) {
      console.error(`Error: Unknown pattern '${patternId}'`);
      console.error(`Run 'blinky-test-player list' to see available patterns`);
      process.exit(1);
    }

    const quiet = options.quiet;
    const delay = parseInt(options.delay, 10);
    const samplesPath = options.samples;

    // Scan for samples
    const manifest = scanSamples(samplesPath);
    const sampleSummary = getSampleSummary(manifest);

    if (Object.keys(manifest).length === 0) {
      console.error('Error: No samples found in samples folder.');
      console.error(`Add .wav/.mp3 files to: ${samplesPath}`);
      console.error('Subfolders: kick, snare, hat, tom, clap, percussion, bass');
      process.exit(1);
    }

    if (!quiet) {
      console.error(`\nPlaying pattern: ${pattern.name}`);
      console.error(`Duration: ${pattern.durationMs / 1000}s, Hits: ${pattern.hits.length}`);
      console.error(`Samples: ${sampleSummary}`);
    }

    // Check if pattern uses instruments we have samples for
    const requiredInstruments = new Set(pattern.hits.map(h => h.instrument).filter(Boolean));
    const availableInstruments = new Set(Object.keys(manifest));
    const missingInstruments = [...requiredInstruments].filter(i => !availableInstruments.has(i!));

    if (missingInstruments.length > 0) {
      console.error(`Error: Missing samples for required instruments: ${missingInstruments.join(', ')}`);
      console.error(`Add samples to: ${missingInstruments.map(i => join(samplesPath, i!)).join(', ')}`);
      process.exit(1);
    }

    // Launch browser
    if (!quiet) console.error('Launching browser...');

    // SECURITY WARNING: These flags disable critical browser security features!
    // --allow-file-access-from-files: Allows file:// URLs to access other local files
    // --disable-web-security: Disables same-origin policy (DANGEROUS!)
    //
    // These flags are ONLY acceptable because:
    // 1. This is a LOCAL TESTING TOOL (not production code)
    // 2. Browser is launched programmatically, not exposed to web
    // 3. Only loads local HTML + WAV files from known safe directories
    //
    // DO NOT use these flags in any production or web-facing context!
    // Alternative for production: Use a local HTTP server instead of file:// URLs
    const browser = await chromium.launch({
      headless: options.headless ?? false,
      args: [
        '--autoplay-policy=no-user-gesture-required',
        '--allow-file-access-from-files',  // Required for file:// → file:// access (WAV loading)
        '--disable-web-security',           // Required for CORS with local files
      ],
    });

    const context = await browser.newContext();
    const page = await context.newPage();

    // Capture console messages
    let startedAt: string | null = null;
    let groundTruth: Array<{
      timeMs: number;
      type: string;
      instrument: string;
      sample: string;
      strength: number;
    }> | null = null;
    let patternComplete = false;

    page.on('console', msg => {
      const text = msg.text();
      if (text.startsWith('PATTERN_STARTED:')) {
        startedAt = text.replace('PATTERN_STARTED:', '');
      } else if (text.startsWith('GROUND_TRUTH:')) {
        try {
          groundTruth = JSON.parse(text.replace('GROUND_TRUTH:', ''));
        } catch { /* ignore */ }
      } else if (text === 'PATTERN_COMPLETE') {
        patternComplete = true;
      } else if (text.startsWith('SAMPLES_LOADED:')) {
        if (!quiet) {
          const info = JSON.parse(text.replace('SAMPLES_LOADED:', ''));
          console.error(`Loaded ${info.total} samples`);
        }
      }
    });

    // Load the player HTML with proper file:// URL format
    const playerHtmlPath = join(__dirname, 'player.html');
    const playerUrl = pathToFileUrl(playerHtmlPath);
    await page.goto(playerUrl);

    // Wait for page to be ready
    await page.waitForFunction(() => (window as unknown as { loadSamples: unknown }).loadSamples !== undefined);

    // Check for curated manifest (deterministic samples with known loudness)
    const curatedManifestPath = join(samplesPath, 'manifest.json');
    let sampleManifest: unknown;

    if (existsSync(curatedManifestPath)) {
      // Use curated manifest for deterministic testing
      if (!quiet) console.error('Using curated sample manifest (deterministic)');
      const curatedManifest = JSON.parse(readFileSync(curatedManifestPath, 'utf-8'));

      // Convert paths to file:// URLs
      sampleManifest = {
        ...curatedManifest,
        samples: curatedManifest.samples.map((s: { newPath: string }) => ({
          ...s,
          newPath: pathToFileUrl(join(samplesPath, '..', s.newPath)),
        })),
      };
    } else {
      // Fall back to legacy folder-based manifest
      if (!quiet) console.error('Using folder-based samples (random selection)');
      sampleManifest = buildManifestWithUrls(samplesPath, manifest);
    }

    // Load samples into the player
    if (!quiet) console.error('Loading samples...');
    await page.evaluate(
      (manifest) => {
        return (window as unknown as { loadSamples: (m: unknown) => Promise<unknown> }).loadSamples(manifest);
      },
      sampleManifest
    );

    if (!quiet) console.error(`Starting in ${delay}ms...`);
    await new Promise(resolve => setTimeout(resolve, delay));

    // Convert pattern hits to player format
    // Supports sampleId for deterministic testing or falls back to instrument type
    const playerHits = pattern.hits.map(h => ({
      timeMs: Math.round(h.time * 1000),
      type: h.instrument || (h.type === 'low' ? 'kick' : 'snare'), // Fallback for old patterns
      sampleId: (h as { sampleId?: string }).sampleId, // Deterministic sample selection
      strength: h.strength,
    }));

    // Play the pattern
    if (!quiet) console.error('Playing...');

    await page.evaluate(
      ({ hits, durationMs }) => {
        return (window as unknown as { playPattern: (hits: unknown[], durationMs: number) => Promise<unknown> }).playPattern(hits, durationMs);
      },
      { hits: playerHits, durationMs: pattern.durationMs }
    );

    // Wait for completion with timeout
    const maxWaitMs = pattern.durationMs + 10000; // Pattern duration + 10s buffer
    const startWait = Date.now();
    while (!patternComplete) {
      if (Date.now() - startWait > maxWaitMs) {
        console.error('Warning: Pattern playback timed out');
        break;
      }
      await new Promise(resolve => setTimeout(resolve, 100));
    }

    await browser.close();

    if (!quiet) console.error('Pattern complete.');

    // Build output using ground truth from player (includes actual samples used)
    const output: PatternOutput = {
      pattern: pattern.id,
      durationMs: pattern.durationMs,
      bpm: pattern.bpm,
      startedAt: startedAt || new Date().toISOString(),
      hits: groundTruth || pattern.hits.map(h => ({
        timeMs: Math.round(h.time * 1000),
        type: h.type,
        instrument: h.instrument,
        strength: h.strength,
      })),
    };

    // Format output
    let outputStr: string;
    if (options.format === 'csv') {
      const lines = [
        `# Pattern: ${pattern.id}`,
        `# Started: ${output.startedAt}`,
        `# Duration: ${output.durationMs}ms`,
        'timeMs,type,instrument,sample,strength',
        ...output.hits.map(h => `${h.timeMs},${h.type},${(h as { instrument?: string }).instrument || ''},${(h as { sample?: string }).sample || ''},${h.strength}`),
      ];
      outputStr = lines.join('\n');
    } else {
      outputStr = JSON.stringify(output, null, 2);
    }

    // Write output
    if (options.output) {
      writeFileSync(options.output, outputStr);
      if (!quiet) console.error(`Ground truth written to: ${options.output}`);
    } else {
      console.log(outputStr);
    }
  });

program.parse();
