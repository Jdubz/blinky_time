/**
 * Fast Parameter Tuning
 * Uses binary search and targeted validation instead of exhaustive sweeps
 * Completes in ~30 min instead of 4-6 hours
 */

import type { DetectionMode, TestResult, TunerOptions } from './types.js';
import { MODE_IDS, PARAMETERS } from './types.js';
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
  mode: DetectionMode;
  params: Record<string, number>;
  f1: number;
  precision: number;
  recall: number;
  byPattern: Record<string, TestResult>;
}

export async function runFastTune(options: TunerOptions): Promise<void> {
  console.log('\n Fast Parameter Tuning');
  console.log('='.repeat(50));
  console.log('Using binary search + targeted validation (~30 min)\n');

  const runner = new TestRunner(options);
  await runner.connect();

  try {
    const results: TuningResult[] = [];

    // Tune each mode
    for (const mode of ['drummer', 'spectral', 'hybrid'] as DetectionMode[]) {
      console.log(`\n${'='.repeat(50)}`);
      console.log(` Tuning ${mode.toUpperCase()} mode`);
      console.log('='.repeat(50));

      const result = await tuneMode(runner, mode);
      results.push(result);

      console.log(`\n Result: F1=${result.f1} | P=${result.precision} | R=${result.recall}`);
      console.log(` Optimal params:`, result.params);
    }

    // Summary
    console.log('\n' + '='.repeat(50));
    console.log(' TUNING COMPLETE');
    console.log('='.repeat(50));

    const best = results.reduce((a, b) => a.f1 > b.f1 ? a : b);
    console.log(`\nBest mode: ${best.mode.toUpperCase()} (F1: ${best.f1})`);
    console.log('\nOptimal parameters by mode:');

    for (const r of results) {
      console.log(`\n${r.mode}:`);
      for (const [param, value] of Object.entries(r.params)) {
        const def = PARAMETERS[param]?.default;
        const change = value !== def ? ` (default: ${def})` : '';
        console.log(`  set ${param} ${value}${change}`);
      }
      console.log(`  -> F1: ${r.f1} | Precision: ${r.precision} | Recall: ${r.recall}`);
    }

    // Save results
    const { writeFileSync, mkdirSync, existsSync } = await import('fs');
    const { join } = await import('path');

    const outputDir = options.outputDir || 'tuning-results';
    if (!existsSync(outputDir)) {
      mkdirSync(outputDir, { recursive: true });
    }

    writeFileSync(
      join(outputDir, 'fast-tune-results.json'),
      JSON.stringify({ timestamp: new Date().toISOString(), results }, null, 2)
    );
    console.log(`\nResults saved to ${join(outputDir, 'fast-tune-results.json')}`);

  } finally {
    await runner.disconnect();
  }
}

async function tuneMode(runner: TestRunner, mode: DetectionMode): Promise<TuningResult> {
  await runner.setMode(mode);
  await runner.resetDefaults(mode);

  // Get the primary threshold parameter for this mode
  const threshParam = mode === 'drummer' ? 'hitthresh'
                    : mode === 'spectral' ? 'fluxthresh'
                    : 'hitthresh';  // hybrid uses drummer's hitthresh

  const threshDef = PARAMETERS[threshParam];

  // Phase 1: Binary search for optimal threshold
  console.log(`\nPhase 1: Finding optimal ${threshParam}...`);
  const optimalThresh = await binarySearchThreshold(
    runner,
    threshParam,
    threshDef.min,
    threshDef.max,
    threshDef.default
  );
  console.log(`  -> Optimal ${threshParam}: ${optimalThresh}`);

  // Apply optimal threshold
  await runner.setParameter(threshParam, optimalThresh);

  // Phase 2: Cross-validation
  console.log('\nPhase 2: Cross-validation on 4 patterns...');
  const crossResults = await testPatterns(runner, CROSS_VALIDATION_PATTERNS);

  const crossF1 = avg(crossResults.map(r => r.f1));
  const crossP = avg(crossResults.map(r => r.precision));
  const crossR = avg(crossResults.map(r => r.recall));
  console.log(`  -> F1: ${crossF1} | P: ${crossP} | R: ${crossR}`);

  // Phase 3: Adjust secondary parameters if needed
  let secondaryParams: Record<string, number> = {};

  if (mode === 'drummer' && crossR < 0.5) {
    // Low recall - try adjusting attackmult
    console.log('\nPhase 3: Adjusting attackmult for better recall...');
    const bestAttack = await quickSweep(runner, 'attackmult', [1.1, 1.2, 1.3, 1.5]);
    secondaryParams.attackmult = bestAttack;
    console.log(`  -> Best attackmult: ${bestAttack}`);
  }

  if (mode === 'drummer') {
    // Quick cooldown check on fast-tempo
    console.log('\nPhase 3: Checking cooldown on fast-tempo...');
    const bestCooldown = await quickSweep(runner, 'cooldown', [40, 60, 80, 100], ['fast-tempo']);
    secondaryParams.cooldown = bestCooldown;
    console.log(`  -> Best cooldown: ${bestCooldown}`);
  }

  if (mode === 'hybrid') {
    // Quick check of hybrid weights
    console.log('\nPhase 3: Optimizing hybrid weights...');
    const bestFluxWt = await quickSweep(runner, 'hyfluxwt', [0.3, 0.5, 0.7, 0.9]);
    const bestDrumWt = await quickSweep(runner, 'hydrumwt', [0.3, 0.5, 0.7, 0.9]);
    secondaryParams.hyfluxwt = bestFluxWt;
    secondaryParams.hydrumwt = bestDrumWt;
    console.log(`  -> Best weights: flux=${bestFluxWt}, drum=${bestDrumWt}`);
  }

  // Apply secondary params
  for (const [param, value] of Object.entries(secondaryParams)) {
    await runner.setParameter(param, value);
  }

  // Phase 4: Final validation
  console.log('\nPhase 4: Final validation on 8 patterns...');
  const finalResults = await testPatterns(runner, FINAL_VALIDATION_PATTERNS);

  const byPattern: Record<string, TestResult> = {};
  for (const r of finalResults) {
    byPattern[r.pattern] = r;
  }

  const f1 = round(avg(finalResults.map(r => r.f1)));
  const precision = round(avg(finalResults.map(r => r.precision)));
  const recall = round(avg(finalResults.map(r => r.recall)));

  // Build final params
  const params: Record<string, number> = { [threshParam]: optimalThresh, ...secondaryParams };

  // Add defaults for params we didn't tune
  const modeParams = Object.values(PARAMETERS).filter(p => p.mode === mode);
  for (const p of modeParams) {
    if (!(p.name in params)) {
      params[p.name] = p.default;
    }
  }

  return { mode, params, f1, precision, recall, byPattern };
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
  console.log(`    ${param}=${defaultVal}: F1=${defaultResult.f1} P=${defaultResult.precision} R=${defaultResult.recall}`);

  if (defaultResult.recall >= targetRecall && defaultResult.precision >= targetPrecision) {
    return defaultVal;  // Default is good enough
  }

  // Binary search - prioritize recall while maintaining precision
  let iterations = 0;
  const maxIterations = 6;

  while (high - low > 0.2 && iterations < maxIterations) {
    const mid = round((low + high) / 2);
    await runner.setParameter(param, mid);
    const result = await runner.runPattern(THRESHOLD_PATTERN);

    console.log(`    ${param}=${mid}: F1=${result.f1} P=${result.precision} R=${result.recall}`);

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
