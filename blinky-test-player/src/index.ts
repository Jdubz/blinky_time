#!/usr/bin/env node

/**
 * Blinky Test Pattern Player CLI
 *
 * Plays known test patterns through system audio using Playwright + Web Audio API.
 * Outputs ground truth timing data for comparison with device detections.
 */

import { chromium } from 'playwright';
import { Command } from 'commander';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { writeFileSync } from 'fs';
import { TEST_PATTERNS, getPatternById } from './patterns.js';
import type { PatternOutput } from './types.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

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
  .command('play <pattern>')
  .description('Play a test pattern')
  .option('-o, --output <file>', 'Output ground truth to file (default: stdout)')
  .option('-f, --format <fmt>', 'Output format: json | csv', 'json')
  .option('-d, --delay <ms>', 'Delay before starting in milliseconds', '500')
  .option('-q, --quiet', 'Suppress progress messages')
  .option('--headless', 'Run browser in headless mode (no visible window)')
  .action(async (patternId: string, options) => {
    const pattern = getPatternById(patternId);

    if (!pattern) {
      console.error(`Error: Unknown pattern '${patternId}'`);
      console.error(`Run 'blinky-test-player list' to see available patterns`);
      process.exit(1);
    }

    const quiet = options.quiet;
    const delay = parseInt(options.delay, 10);

    if (!quiet) {
      console.error(`\nPlaying pattern: ${pattern.name}`);
      console.error(`Duration: ${pattern.durationMs / 1000}s, Hits: ${pattern.hits.length}`);
    }

    // Launch browser
    if (!quiet) console.error('Launching browser...');

    const browser = await chromium.launch({
      headless: options.headless ?? false,
      args: [
        '--autoplay-policy=no-user-gesture-required', // Allow audio without user gesture
      ],
    });

    const context = await browser.newContext();
    const page = await context.newPage();

    // Capture console messages
    let startedAt: string | null = null;
    let patternComplete = false;

    page.on('console', msg => {
      const text = msg.text();
      if (text.startsWith('PATTERN_STARTED:')) {
        startedAt = text.replace('PATTERN_STARTED:', '');
      } else if (text === 'PATTERN_COMPLETE') {
        patternComplete = true;
      }
    });

    // Load the player HTML
    const playerPath = join(__dirname, 'player.html');
    await page.goto(`file://${playerPath}`);

    // Wait for page to be ready
    await page.waitForFunction(() => (window as unknown as { playPattern: unknown }).playPattern !== undefined);

    if (!quiet) console.error(`Starting in ${delay}ms...`);
    await new Promise(resolve => setTimeout(resolve, delay));

    // Play the pattern
    if (!quiet) console.error('Playing...');

    await page.evaluate(
      ({ hits, durationMs, background }) => {
        return (window as unknown as { playPattern: (hits: unknown[], durationMs: number, background: unknown) => Promise<string> }).playPattern(hits, durationMs, background);
      },
      { hits: pattern.hits, durationMs: pattern.durationMs, background: pattern.background || null }
    );

    // Wait for completion
    while (!patternComplete) {
      await new Promise(resolve => setTimeout(resolve, 100));
    }

    await browser.close();

    if (!quiet) console.error('Pattern complete.');

    // Build output
    const output: PatternOutput = {
      pattern: pattern.id,
      durationMs: pattern.durationMs,
      startedAt: startedAt || new Date().toISOString(),
      hits: pattern.hits.map(h => ({
        timeMs: Math.round(h.time * 1000),
        type: h.type,
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
        'timeMs,type,strength',
        ...output.hits.map(h => `${h.timeMs},${h.type},${h.strength}`),
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
