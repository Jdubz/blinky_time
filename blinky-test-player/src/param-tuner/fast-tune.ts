/**
 * Fast Parameter Tuning for Ensemble Detector
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * This tuner optimizes detector thresholds, weights, and agreement boosts.
 *
 * Tuning Strategy:
 * Phase 1: Find optimal threshold for each detector
 * Phase 2: Optimize detector weights
 * Phase 3: Tune agreement boost values
 * Phase 4: Final validation
 */

import type { TestResult, TunerOptions, DetectorType } from './types.js';
import { PARAMETERS, DETECTOR_TYPES } from './types.js';
import { TestRunner } from './runner.js';

// Diagnostic patterns for each phase
const THRESHOLD_PATTERN = 'strong-beats';  // Clear transients, 32 expected hits

const CROSS_VALIDATION_PATTERNS = [
  'strong-beats',   // Percussion baseline
  'bass-line',      // Low frequency
  'synth-stabs',    // Sharp melodic transients
  'pad-rejection',  // False positive test
];

const FINAL_VALIDATION_PATTERNS = [
  'strong-beats',
  'sparse',
  'bass-line',
  'synth-stabs',
  'pad-rejection',
  'fast-tempo',
  'simultaneous',
  'full-mix',
];

interface TuningResult {
  thresholds: Record<DetectorType, number>;
  weights: Record<DetectorType, number>;
  agreementBoosts: number[];
  f1: number;
  precision: number;
  recall: number;
  byPattern: Record<string, TestResult>;
}

export async function runFastTune(options: TunerOptions): Promise<void> {
  console.log('\n Ensemble Fast Parameter Tuning');
  console.log('='.repeat(50));
  console.log('Optimizing 6 detectors + agreement boosts (~30 min)\n');

  const runner = new TestRunner(options);
  await runner.connect();

  try {
    // Reset to defaults
    await runner.resetDefaults();

    // Phase 1: Tune each detector's threshold
    console.log('\n' + '='.repeat(50));
    console.log(' Phase 1: Tuning Detector Thresholds');
    console.log('='.repeat(50));

    const thresholds: Record<string, number> = {};
    for (const detector of DETECTOR_TYPES) {
      const paramName = `${detector}_thresh`;
      const param = PARAMETERS[paramName];
      if (!param) {
        console.log(`  Skipping ${detector} (no threshold param)`);
        continue;
      }

      console.log(`\n  Tuning ${detector} threshold...`);
      const optimal = await binarySearchThreshold(
        runner,
        paramName,
        param.min,
        param.max,
        param.default
      );
      thresholds[detector] = optimal;
      console.log(`    -> Optimal ${detector}_thresh: ${optimal}`);
    }

    // Phase 2: Optimize detector weights
    console.log('\n' + '='.repeat(50));
    console.log(' Phase 2: Optimizing Detector Weights');
    console.log('='.repeat(50));

    const weights: Record<string, number> = {};
    for (const detector of DETECTOR_TYPES) {
      const paramName = `${detector}_weight`;
      const param = PARAMETERS[paramName];
      if (!param) {
        continue;
      }
      // Start with default, will adjust in grid search
      weights[detector] = param.default;
    }

    // Quick weight sweep for top 3 detectors
    for (const detector of ['drummer', 'spectral', 'bass'] as DetectorType[]) {
      console.log(`\n  Tuning ${detector} weight...`);
      const paramName = `${detector}_weight`;
      const param = PARAMETERS[paramName];
      const bestWeight = await quickSweep(runner, paramName, param.sweepValues.slice(0, 5));
      weights[detector] = bestWeight;
      console.log(`    -> Optimal ${detector}_weight: ${bestWeight}`);
    }

    // Phase 3: Tune agreement boosts
    console.log('\n' + '='.repeat(50));
    console.log(' Phase 3: Tuning Agreement Boosts');
    console.log('='.repeat(50));

    const agreementBoosts = [0.0, 0.6, 0.85, 1.0, 1.1, 1.15, 1.2];  // Defaults

    // Focus on agree_1 and agree_2 (most impactful for false positive suppression)
    console.log('\n  Tuning agree_1 (single detector boost)...');
    const bestAgree1 = await quickSweep(runner, 'agree_1', [0.4, 0.5, 0.6, 0.7, 0.8], ['hat-rejection', 'pad-rejection']);
    agreementBoosts[1] = bestAgree1;
    console.log(`    -> Optimal agree_1: ${bestAgree1}`);

    console.log('\n  Tuning agree_2 (two detector boost)...');
    const bestAgree2 = await quickSweep(runner, 'agree_2', [0.7, 0.8, 0.85, 0.9, 0.95], CROSS_VALIDATION_PATTERNS.slice(0, 2));
    agreementBoosts[2] = bestAgree2;
    console.log(`    -> Optimal agree_2: ${bestAgree2}`);

    // Phase 4: Final validation
    console.log('\n' + '='.repeat(50));
    console.log(' Phase 4: Final Validation');
    console.log('='.repeat(50));

    console.log('\n  Running 8 validation patterns...');
    const finalResults = await testPatterns(runner, FINAL_VALIDATION_PATTERNS);

    const byPattern: Record<string, TestResult> = {};
    for (const r of finalResults) {
      byPattern[r.pattern] = r;
    }

    const f1 = round(avg(finalResults.map(r => r.f1)));
    const precision = round(avg(finalResults.map(r => r.precision)));
    const recall = round(avg(finalResults.map(r => r.recall)));

    // Build result
    const result: TuningResult = {
      thresholds: thresholds as Record<DetectorType, number>,
      weights: weights as Record<DetectorType, number>,
      agreementBoosts,
      f1,
      precision,
      recall,
      byPattern,
    };

    // Summary
    console.log('\n' + '='.repeat(50));
    console.log(' TUNING COMPLETE');
    console.log('='.repeat(50));

    console.log(`\nOverall: F1=${f1} | Precision=${precision} | Recall=${recall}`);

    console.log('\nOptimal Thresholds:');
    for (const [det, val] of Object.entries(thresholds)) {
      console.log(`  set detector_thresh ${det} ${val}`);
    }

    console.log('\nOptimal Weights:');
    for (const [det, val] of Object.entries(weights)) {
      console.log(`  set detector_weight ${det} ${val}`);
    }

    console.log('\nOptimal Agreement Boosts:');
    for (let i = 1; i <= 6; i++) {
      console.log(`  set agree_${i} ${agreementBoosts[i]}`);
    }

    // Save results
    const { writeFileSync, mkdirSync, existsSync } = await import('fs');
    const { join } = await import('path');

    const outputDir = options.outputDir || 'tuning-results';
    if (!existsSync(outputDir)) {
      mkdirSync(outputDir, { recursive: true });
    }

    writeFileSync(
      join(outputDir, 'ensemble-tune-results.json'),
      JSON.stringify({ timestamp: new Date().toISOString(), result }, null, 2)
    );
    console.log(`\nResults saved to ${join(outputDir, 'ensemble-tune-results.json')}`);

  } finally {
    await runner.disconnect();
  }
}

async function binarySearchThreshold(
  runner: TestRunner,
  param: string,
  min: number,
  max: number,
  defaultVal: number,
  targetRecall = 0.7,
  targetPrecision = 0.8
): Promise<number> {
  let low = min;
  let high = defaultVal * 1.5;  // Start search up to 1.5x default
  let best = defaultVal;
  let bestScore = 0;

  // First, test default
  await runner.setParameter(param, defaultVal);
  const defaultResult = await runner.runPattern(THRESHOLD_PATTERN);
  console.log(`      ${param}=${defaultVal}: F1=${defaultResult.f1} P=${defaultResult.precision} R=${defaultResult.recall}`);

  if (defaultResult.recall >= targetRecall && defaultResult.precision >= targetPrecision) {
    return defaultVal;  // Default is good enough
  }

  // Binary search - prioritize recall while maintaining precision
  let iterations = 0;
  const maxIterations = 5;

  while (high - low > 0.2 && iterations < maxIterations) {
    const mid = round((low + high) / 2);
    await runner.setParameter(param, mid);
    const result = await runner.runPattern(THRESHOLD_PATTERN);

    console.log(`      ${param}=${mid}: F1=${result.f1} P=${result.precision} R=${result.recall}`);

    const score = result.f1;  // Optimize for F1
    if (score > bestScore) {
      bestScore = score;
      best = mid;
    }

    // Adjust search range based on results
    if (result.recall < targetRecall) {
      high = mid;  // Need lower threshold for more sensitivity
    } else if (result.precision < targetPrecision) {
      low = mid;   // Need higher threshold for fewer false positives
    } else {
      // Both targets met - try to optimize further
      if (result.recall > result.precision) {
        low = mid;  // Can afford to raise threshold
      } else {
        high = mid; // Try lower for better recall
      }
    }

    iterations++;
  }

  // Apply best value
  await runner.setParameter(param, best);
  return best;
}

async function quickSweep(
  runner: TestRunner,
  param: string,
  values: number[],
  patterns: string[] = CROSS_VALIDATION_PATTERNS.slice(0, 2)
): Promise<number> {
  let bestValue = values[0];
  let bestF1 = 0;

  for (const value of values) {
    await runner.setParameter(param, value);

    let totalF1 = 0;
    for (const pattern of patterns) {
      const result = await runner.runPattern(pattern);
      totalF1 += result.f1;
    }

    const avgF1 = totalF1 / patterns.length;
    if (avgF1 > bestF1) {
      bestF1 = avgF1;
      bestValue = value;
    }
  }

  await runner.setParameter(param, bestValue);
  return bestValue;
}

async function testPatterns(runner: TestRunner, patterns: string[]): Promise<TestResult[]> {
  const results: TestResult[] = [];

  for (const pattern of patterns) {
    try {
      const result = await runner.runPattern(pattern);
      results.push(result);
      console.log(`    ${pattern}: F1=${result.f1} P=${result.precision} R=${result.recall}`);
    } catch (err) {
      console.error(`    ${pattern}: ERROR - ${err}`);
    }
  }

  return results;
}

function avg(nums: number[]): number {
  if (nums.length === 0) return 0;
  return nums.reduce((a, b) => a + b, 0) / nums.length;
}

function round(n: number): number {
  return Math.round(n * 1000) / 1000;
}
