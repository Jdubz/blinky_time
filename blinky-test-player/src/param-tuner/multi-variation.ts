/**
 * Multi-device variation testing
 *
 * Runs all patterns with identical settings on multiple devices simultaneously.
 * Produces per-device comparison to measure inter-device variation:
 * - Per-device F1/precision/recall per pattern
 * - Cross-device stddev, min, max, spread
 * - Identifies consistent vs divergent patterns
 */

import cliProgress from 'cli-progress';
import type { MultiDeviceTunerOptions, MultiDeviceTestResult } from './types.js';
import { REPRESENTATIVE_PATTERNS } from './types.js';
import { MultiDeviceRunner } from './multi-device-runner.js';

export interface VariationReport {
  timestamp: string;
  ports: string[];
  patterns: MultiDeviceTestResult[];
  summary: {
    avgF1PerDevice: Record<string, number>;
    avgF1Spread: number;
    consistentPatterns: string[];
    divergentPatterns: string[];
  };
}

/**
 * Run variation testing across multiple devices.
 * All devices run with identical (default) settings; measures how much results vary.
 */
export async function runVariation(options: MultiDeviceTunerOptions): Promise<VariationReport> {
  console.log('\n Multi-Device Variation Test');
  console.log('='.repeat(50));
  console.log(`Devices: ${options.ports.length} (${options.ports.join(', ')})`);

  const runner = new MultiDeviceRunner(options.ports, {
    gain: options.gain,
    recordAudio: options.recordAudio,
  });

  try {
    await runner.connectAll();

    // Reset all devices to defaults
    await runner.resetDefaultsAll();

    const patterns = (options.patterns && options.patterns.length > 0)
      ? options.patterns
      : [...REPRESENTATIVE_PATTERNS];

    console.log(`Patterns: ${patterns.length}`);
    console.log('');

    const progress = new cliProgress.SingleBar({
      format: '   {bar} {percentage}% | {value}/{total} patterns | {eta_formatted}',
      barCompleteChar: '\u2588',
      barIncompleteChar: '\u2591',
      hideCursor: true,
    });

    progress.start(patterns.length, 0);

    const results: MultiDeviceTestResult[] = [];

    for (const pattern of patterns) {
      try {
        const result = await runner.runPatternAllDevices(pattern);
        results.push(result);
      } catch (err) {
        console.error(`\n   Error on ${pattern}:`, err);
      }
      progress.increment();
    }

    progress.stop();

    // Build summary
    const avgF1PerDevice: Record<string, number> = {};
    for (const port of options.ports) {
      const deviceResults = results
        .flatMap(r => r.perDevice)
        .filter(d => d.port === port);
      const sum = deviceResults.reduce((a, d) => a + d.result.f1, 0);
      avgF1PerDevice[port] = deviceResults.length > 0
        ? Math.round((sum / deviceResults.length) * 1000) / 1000
        : 0;
    }

    const f1Values = Object.values(avgF1PerDevice);
    const avgF1Spread = f1Values.length > 1
      ? Math.round((Math.max(...f1Values) - Math.min(...f1Values)) * 1000) / 1000
      : 0;

    // Categorize patterns by consistency
    const DIVERGENCE_THRESHOLD = 0.15;
    const consistentPatterns: string[] = [];
    const divergentPatterns: string[] = [];

    for (const result of results) {
      if (result.variation) {
        if (result.variation.f1.spread <= DIVERGENCE_THRESHOLD) {
          consistentPatterns.push(result.pattern);
        } else {
          divergentPatterns.push(result.pattern);
        }
      }
    }

    const report: VariationReport = {
      timestamp: new Date().toISOString(),
      ports: options.ports,
      patterns: results,
      summary: {
        avgF1PerDevice,
        avgF1Spread,
        consistentPatterns,
        divergentPatterns,
      },
    };

    // Print results
    console.log('\n Variation Results');
    console.log('='.repeat(60));

    // Per-device averages
    console.log('\nPer-Device Average F1:');
    for (const [port, f1] of Object.entries(avgF1PerDevice)) {
      const idx = options.ports.indexOf(port);
      console.log(`  D${idx + 1} (${port}): ${f1}`);
    }
    console.log(`  Spread: ${avgF1Spread}`);

    // Per-pattern breakdown
    console.log('\nPer-Pattern Breakdown:');
    const header = ['Pattern', ...options.ports.map((_, i) => `D${i + 1} F1`), 'Spread'].join(' | ');
    console.log(`  ${header}`);
    console.log(`  ${'-'.repeat(header.length)}`);

    for (const result of results) {
      const f1s = result.perDevice.map(d => d.result.f1.toFixed(3));
      const spread = result.variation ? result.variation.f1.spread.toFixed(3) : 'N/A';
      const row = [result.pattern.padEnd(25), ...f1s.map(f => f.padStart(6)), spread.padStart(6)].join(' | ');
      console.log(`  ${row}`);
    }

    if (divergentPatterns.length > 0) {
      console.log(`\nDivergent patterns (spread > ${DIVERGENCE_THRESHOLD}):`);
      for (const p of divergentPatterns) {
        console.log(`  - ${p}`);
      }
    }

    if (consistentPatterns.length > 0) {
      console.log(`\nConsistent patterns (spread <= ${DIVERGENCE_THRESHOLD}): ${consistentPatterns.length}/${results.length}`);
    }

    return report;

  } finally {
    await runner.disconnectAll();
  }
}
