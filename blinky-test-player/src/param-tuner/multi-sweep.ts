/**
 * Multi-device parallel parameter sweep
 *
 * Batches sweep values across N devices to reduce total playback count.
 * For a param with 10 sweep values and 3 devices:
 *   Batch 1: D1=v1, D2=v2, D3=v3 -> play once -> 3 results
 *   Batch 2: D1=v4, D2=v5, D3=v6 -> play once -> 3 results
 *   Batch 3: D1=v7, D2=v8, D3=v9 -> play once -> 3 results
 *   Batch 4: D1=v10              -> play once -> 1 result
 *   Total: 4 playbacks instead of 10 (2.5x speedup)
 */

import cliProgress from 'cli-progress';
import type { MultiDeviceTunerOptions, TestResult, SweepPoint } from './types.js';
import { PARAMETERS, REPRESENTATIVE_PATTERNS, generateSweepValues } from './types.js';
import { MultiDeviceRunner } from './multi-device-runner.js';

export interface MultiSweepResult {
  parameter: string;
  timestamp: string;
  sweep: SweepPoint[];
  optimal: { value: number; avgF1: number };
  deviceCount: number;
  playbackCount: number;
  speedup: string;
}

/**
 * Run a parallel parameter sweep across multiple devices.
 */
export async function runMultiSweep(options: MultiDeviceTunerOptions): Promise<MultiSweepResult[]> {
  console.log('\n Multi-Device Parallel Sweep');
  console.log('='.repeat(50));
  console.log(`Devices: ${options.ports.length} (${options.ports.join(', ')})`);

  const runner = new MultiDeviceRunner(options.ports, {
    gain: options.gain,
    recordAudio: options.recordAudio,
  });

  try {
    await runner.connectAll();

    // Determine which parameters to sweep
    const allParams = Object.values(PARAMETERS);
    let paramsToSweep = allParams;

    if (options.params && options.params.length > 0) {
      paramsToSweep = allParams.filter(p => options.params!.includes(p.name));
    }
    if (options.modes && options.modes.length > 0) {
      paramsToSweep = paramsToSweep.filter(p => options.modes!.includes(p.mode));
    }

    const patterns = (options.patterns && options.patterns.length > 0)
      ? options.patterns
      : [...REPRESENTATIVE_PATTERNS];

    console.log(`Parameters: ${paramsToSweep.length}`);
    console.log(`Patterns per value: ${patterns.length}`);
    console.log('');

    const results: MultiSweepResult[] = [];

    for (const param of paramsToSweep) {
      console.log(`\nSweeping ${param.name} (${param.mode})...`);

      // Reset all devices to defaults before each parameter
      await runner.resetDefaultsAll();

      const sweepValues = generateSweepValues(param);
      const deviceCount = runner.deviceCount;
      const ports = runner.portList;

      // Batch values across devices
      const batches: Array<Map<string, number>> = [];
      for (let i = 0; i < sweepValues.length; i += deviceCount) {
        const batch = new Map<string, number>();
        for (let d = 0; d < deviceCount && i + d < sweepValues.length; d++) {
          batch.set(ports[d], sweepValues[i + d]);
        }
        batches.push(batch);
      }

      const playbackCount = batches.length * patterns.length;
      const singleDeviceCount = sweepValues.length * patterns.length;
      const speedup = singleDeviceCount > 0
        ? (singleDeviceCount / playbackCount).toFixed(1)
        : '1.0';

      console.log(`   Values: ${sweepValues.join(', ')}`);
      console.log(`   Batches: ${batches.length} (${speedup}x speedup vs single-device)`);

      // Progress bar
      const progress = new cliProgress.SingleBar({
        format: '   {bar} {percentage}% | {value}/{total} playbacks | {eta_formatted}',
        barCompleteChar: '\u2588',
        barIncompleteChar: '\u2591',
        hideCursor: true,
      });
      progress.start(playbackCount, 0);

      // Accumulate results per sweep value
      const resultsByValue = new Map<number, { byPattern: Record<string, TestResult>; totalF1: number; totalP: number; totalR: number; count: number }>();

      for (const value of sweepValues) {
        resultsByValue.set(value, { byPattern: {}, totalF1: 0, totalP: 0, totalR: 0, count: 0 });
      }

      for (const batch of batches) {
        // Run each pattern (runPatternWithAssignments sets per-device values internally)
        for (const pattern of patterns) {
          try {
            const valueResults = await runner.runPatternWithAssignments(
              pattern,
              param.name,
              batch,
            );

            // Store results keyed by value
            for (const [value, testResult] of valueResults) {
              const acc = resultsByValue.get(value)!;
              acc.byPattern[pattern] = testResult;
              acc.totalF1 += testResult.f1;
              acc.totalP += testResult.precision;
              acc.totalR += testResult.recall;
              acc.count++;
            }
          } catch (err) {
            console.error(`\n   Error on ${pattern}:`, err);
          }
          progress.increment();
        }
      }

      progress.stop();

      // Build sweep points
      const sweepPoints: SweepPoint[] = [];
      for (const value of sweepValues) {
        const acc = resultsByValue.get(value)!;
        const n = acc.count;
        if (n > 0) {
          sweepPoints.push({
            value,
            avgF1: Math.round((acc.totalF1 / n) * 1000) / 1000,
            avgPrecision: Math.round((acc.totalP / n) * 1000) / 1000,
            avgRecall: Math.round((acc.totalR / n) * 1000) / 1000,
            byPattern: acc.byPattern,
          });
        }
      }

      // Find optimal
      if (sweepPoints.length === 0) {
        console.error(`   No data collected for ${param.name}, skipping`);
        continue;
      }

      let optimalPoint = sweepPoints[0];
      for (const point of sweepPoints) {
        if (point.avgF1 > optimalPoint.avgF1) {
          optimalPoint = point;
        }
      }

      const sweepResult: MultiSweepResult = {
        parameter: param.name,
        timestamp: new Date().toISOString(),
        sweep: sweepPoints,
        optimal: { value: optimalPoint.value, avgF1: optimalPoint.avgF1 },
        deviceCount,
        playbackCount,
        speedup,
      };

      results.push(sweepResult);

      console.log(`   Optimal: ${optimalPoint.value} (F1: ${optimalPoint.avgF1})`);

      // Reset parameter to default before next
      await runner.setParameterAll(param.name, param.default);
    }

    // Print summary
    console.log('\n Multi-Sweep Summary');
    console.log('='.repeat(50));

    for (const r of results) {
      const paramDef = PARAMETERS[r.parameter];
      const change = r.optimal.value !== paramDef?.default
        ? ` (default: ${paramDef?.default})`
        : '';
      console.log(`  ${r.parameter}: ${r.optimal.value}${change} -> F1: ${r.optimal.avgF1} (${r.speedup}x speedup)`);
    }

    return results;

  } finally {
    await runner.disconnectAll();
  }
}
