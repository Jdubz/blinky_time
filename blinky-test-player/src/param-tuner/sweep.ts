/**
 * Phase 2: Parameter Sweeps
 * Sweeps each parameter independently to find optimal values
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy per-mode sweeping has been removed.
 */

import cliProgress from 'cli-progress';
import type { ParameterMode, SweepResult, SweepPoint, TunerOptions, ParameterDef, TestResult } from './types.js';
import { PARAMETERS, REPRESENTATIVE_PATTERNS } from './types.js';
import { StateManager } from './state.js';
import { TestRunner } from './runner.js';
import { ResultLogger } from './result-logger.js';

/**
 * Filter parameters based on options.params and options.modes
 */
function filterParameters(allParams: ParameterDef[], options: TunerOptions): ParameterDef[] {
  let filtered = allParams;

  // Filter by mode if specified
  if (options.modes && options.modes.length > 0) {
    filtered = filtered.filter(p => options.modes!.includes(p.mode));
  }

  // Filter by specific parameter names if specified
  if (options.params && options.params.length > 0) {
    filtered = filtered.filter(p => options.params!.includes(p.name));
  }

  return filtered;
}

export async function runSweeps(
  options: TunerOptions,
  stateManager: StateManager
): Promise<void> {
  console.log('\n Phase 2: Parameter Sweeps');
  console.log('='.repeat(50));
  console.log('Sweeping each parameter to find optimal values.\n');

  const runner = new TestRunner(options);
  const resultLogger = new ResultLogger(options.outputDir!);
  await runner.connect();

  try {
    const allParams = Object.values(PARAMETERS);
    const params = filterParameters(allParams, options);

    // Show filtering info if filters are active
    if (options.params || options.modes) {
      console.log(`Filtering: ${params.length} of ${allParams.length} parameters selected`);
      if (options.modes) console.log(`   Modes: ${options.modes.join(', ')}`);
      if (options.params) console.log(`   Params: ${options.params.join(', ')}`);
      console.log();
    }

    // Use specified patterns or all representative patterns
    const patterns = (options.patterns && options.patterns.length > 0)
      ? options.patterns
      : [...REPRESENTATIVE_PATTERNS];

    for (const param of params) {
      if (stateManager.isSweepComplete(param.name)) {
        const existing = stateManager.getSweepResult(param.name);
        if (existing) {
          console.log(`${param.name}: Already complete (optimal: ${existing.optimal.value}, F1: ${existing.optimal.avgF1})`);
        }
        continue;
      }

      console.log(`\nSweeping ${param.name} (${param.mode})...`);
      console.log(`   Values: ${param.sweepValues.join(', ')}`);

      // Reset to defaults before sweeping
      await runner.resetDefaults();

      // Load any partial results from previous interrupted run
      const existingProgress = stateManager.getIncrementalSweepProgress(param.name);
      const sweepPoints: SweepPoint[] = existingProgress?.partialResults || [];
      const resumeValueIndex = existingProgress?.valueIndex ?? 0;
      const resumePatternIndex = existingProgress?.patternIndex ?? 0;

      // Calculate progress for display
      const totalTests = param.sweepValues.length * patterns.length;
      const completedTests = (sweepPoints.length * patterns.length) + resumePatternIndex;

      // Progress bar
      const progress = new cliProgress.SingleBar({
        format: '   {bar} {percentage}% | {value}/{total} tests | {eta_formatted}',
        barCompleteChar: '\u2588',
        barIncompleteChar: '\u2591',
        hideCursor: true,
      });
      progress.start(totalTests, completedTests);

      // Log resume info if resuming mid-sweep
      if (resumeValueIndex > 0 || resumePatternIndex > 0) {
        console.log(`   Resuming from value ${resumeValueIndex + 1}/${param.sweepValues.length}, pattern ${resumePatternIndex + 1}/${patterns.length}`);
      }

      for (let i = resumeValueIndex; i < param.sweepValues.length; i++) {
        const value = param.sweepValues[i];
        stateManager.setSweepInProgress(param.name, i);

        // Set the parameter value
        await runner.setParameter(param.name, value);

        // Run all representative patterns
        const byPattern: Record<string, TestResult> = {};
        let totalF1 = 0;
        let totalPrecision = 0;
        let totalRecall = 0;

        // Determine starting pattern index (only non-zero for first value when resuming)
        const startPatternIndex = (i === resumeValueIndex) ? resumePatternIndex : 0;

        // Restore partial pattern results for current value if resuming
        if (i === resumeValueIndex && existingProgress?.currentValueResults) {
          for (let p = 0; p < existingProgress.currentValueResults.length && p < patterns.length; p++) {
            const patternName = patterns[p];
            const result = existingProgress.currentValueResults[p];
            byPattern[patternName] = result;
            totalF1 += result.f1;
            totalPrecision += result.precision;
            totalRecall += result.recall;
          }
        }

        for (let j = startPatternIndex; j < patterns.length; j++) {
          const pattern = patterns[j];

          try {
            const result = await runner.runPattern(pattern);
            byPattern[pattern] = result;
            totalF1 += result.f1;
            totalPrecision += result.precision;
            totalRecall += result.recall;

            // INCREMENTAL SAVE: Save after each pattern completes
            stateManager.appendPatternResult(param.name, i, value, result);

          } catch (err) {
            console.error(`\n   Error on ${pattern}:`, err);
          }
          progress.increment();
        }

        const n = Object.keys(byPattern).length;
        if (n > 0) {
          const sweepPoint: SweepPoint = {
            value,
            avgF1: Math.round((totalF1 / n) * 1000) / 1000,
            avgPrecision: Math.round((totalPrecision / n) * 1000) / 1000,
            avgRecall: Math.round((totalRecall / n) * 1000) / 1000,
            byPattern,
          };
          sweepPoints.push(sweepPoint);

          // INCREMENTAL SAVE: Finalize this value (moves to partialResults, resets currentValueResults)
          stateManager.finalizeSweepValue(param.name, sweepPoint);
        }
      }

      progress.stop();

      // Find optimal value
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

      const result: SweepResult = {
        parameter: param.name,
        mode: param.mode,
        timestamp: new Date().toISOString(),
        sweep: sweepPoints,
        optimal: {
          value: optimalPoint.value,
          avgF1: optimalPoint.avgF1,
        },
      };

      stateManager.saveSweepResult(param.name, result);

      // Clear incremental progress now that sweep is complete
      stateManager.clearIncrementalProgress(param.name);

      console.log(`   Optimal: ${optimalPoint.value} (F1: ${optimalPoint.avgF1})`);

      // Perform adaptive refinement if requested
      let refinementUsed = false;
      if (options.refine) {
        const refinementSteps = options.refinementSteps || 3;
        console.log(`\n   Refining ${param.name} (${refinementSteps} steps)...`);

        const refinedResult = await performAdaptiveRefinement(
          param,
          result,
          patterns,
          refinementSteps,
          runner
        );

        // Update result with refined optimal
        result.optimal = refinedResult.optimal;
        result.sweep = [...result.sweep, ...refinedResult.refinementPoints];
        stateManager.saveSweepResult(param.name, result);

        console.log(`   Refined Optimal: ${refinedResult.optimal.value} (F1: ${refinedResult.optimal.avgF1})`);
        refinementUsed = true;
      }

      // Log result to JSON log
      await resultLogger.logSweepResult(result, refinementUsed);

      // Reset to default before next parameter
      await runner.setParameter(param.name, param.default);
    }

    // Calculate optimal params for ensemble
    updateOptimalParams(stateManager);

    stateManager.markSweepPhaseComplete();
    console.log('\n Sweep phase complete.\n');

  } finally {
    await runner.disconnect();
  }
}

/**
 * Adaptive Refinement: Test finer-grained values around the optimal
 *
 * Algorithm:
 * 1. Find the optimal value and its neighbors from the initial sweep
 * 2. For each refinement step:
 *    - Calculate the range between neighbors
 *    - Test midpoints in that range
 *    - Update optimal if a better value is found
 *    - Narrow the search range for next iteration
 */
async function performAdaptiveRefinement(
  param: ParameterDef,
  initialResult: SweepResult,
  patterns: string[],
  steps: number,
  runner: TestRunner
): Promise<{
  optimal: { value: number; avgF1: number };
  refinementPoints: SweepPoint[];
}> {
  const allPoints: SweepPoint[] = [...initialResult.sweep];
  let currentOptimal = initialResult.optimal;

  for (let step = 0; step < steps; step++) {
    // Find optimal and its neighbors
    const optimalIdx = allPoints.findIndex(p => p.value === currentOptimal.value);
    if (optimalIdx === -1) break;

    const leftNeighbor = optimalIdx > 0 ? allPoints[optimalIdx - 1] : null;
    const rightNeighbor = optimalIdx < allPoints.length - 1 ? allPoints[optimalIdx + 1] : null;

    // Generate test values between neighbors
    const testValues: number[] = [];

    if (leftNeighbor) {
      const gap = currentOptimal.value - leftNeighbor.value;
      const midpoint = leftNeighbor.value + gap / 2;

      // Only test if midpoint is sufficiently different and within bounds
      if (Math.abs(midpoint - currentOptimal.value) > 0.001 &&
          midpoint >= param.min && midpoint <= param.max) {
        testValues.push(midpoint);
      }
    }

    if (rightNeighbor) {
      const gap = rightNeighbor.value - currentOptimal.value;
      const midpoint = currentOptimal.value + gap / 2;

      // Only test if midpoint is sufficiently different and within bounds
      if (Math.abs(midpoint - currentOptimal.value) > 0.001 &&
          midpoint >= param.min && midpoint <= param.max) {
        testValues.push(midpoint);
      }
    }

    // If no values to test, we're done refining
    if (testValues.length === 0) {
      console.log(`      Step ${step + 1}/${steps}: No refinement values to test (converged)`);
      break;
    }

    console.log(`      Step ${step + 1}/${steps}: Testing ${testValues.length} value(s): ${testValues.map(v => v.toFixed(3)).join(', ')}`);

    // Test each value
    for (const value of testValues) {
      await runner.setParameter(param.name, value);

      const byPattern: Record<string, TestResult> = {};
      let totalF1 = 0;
      let totalPrecision = 0;
      let totalRecall = 0;

      for (const pattern of patterns) {
        try {
          const result = await runner.runPattern(pattern);
          byPattern[pattern] = result;
          totalF1 += result.f1;
          totalPrecision += result.precision;
          totalRecall += result.recall;
        } catch (err) {
          console.error(`\n      Error on ${pattern}:`, err);
        }
      }

      const n = Object.keys(byPattern).length;
      if (n > 0) {
        const sweepPoint: SweepPoint = {
          value,
          avgF1: Math.round((totalF1 / n) * 1000) / 1000,
          avgPrecision: Math.round((totalPrecision / n) * 1000) / 1000,
          avgRecall: Math.round((totalRecall / n) * 1000) / 1000,
          byPattern,
        };
        allPoints.push(sweepPoint);

        // Update optimal if this is better
        if (sweepPoint.avgF1 > currentOptimal.avgF1) {
          currentOptimal = {
            value: sweepPoint.value,
            avgF1: sweepPoint.avgF1,
          };
          console.log(`      New optimal: ${value.toFixed(3)} (F1: ${sweepPoint.avgF1}, +${(sweepPoint.avgF1 - initialResult.optimal.avgF1).toFixed(3)})`);
        }
      }
    }

    // Sort points for next iteration
    allPoints.sort((a, b) => a.value - b.value);
  }

  // Return refined optimal and new points tested
  const refinementPoints = allPoints.filter(
    p => !initialResult.sweep.some(sp => sp.value === p.value)
  );

  return {
    optimal: currentOptimal,
    refinementPoints,
  };
}

function updateOptimalParams(stateManager: StateManager): void {
  const optimalParams: Record<string, number> = {};

  // Collect optimal values from all sweeps
  for (const param of Object.values(PARAMETERS)) {
    const sweepResult = stateManager.getSweepResult(param.name);
    if (sweepResult) {
      optimalParams[param.name] = sweepResult.optimal.value;
    }
  }

  if (Object.keys(optimalParams).length > 0) {
    stateManager.setOptimalParams(optimalParams);
  }
}

export async function showSweepSummary(stateManager: StateManager): Promise<void> {
  console.log('\n Sweep Summary');
  console.log('='.repeat(50));

  const params = Object.values(PARAMETERS);
  const byMode: Record<ParameterMode, Array<{ param: string; default: number; optimal: number; f1: number }>> = {
    ensemble: [],
    music: [],
    rhythm: [],
  };

  for (const param of params) {
    const result = stateManager.getSweepResult(param.name);
    if (result) {
      byMode[param.mode].push({
        param: param.name,
        default: param.default,
        optimal: result.optimal.value,
        f1: result.optimal.avgF1,
      });
    }
  }

  for (const [mode, results] of Object.entries(byMode)) {
    console.log(`\n${mode.toUpperCase()}`);
    if (results.length === 0) {
      console.log('  No sweeps completed');
    } else {
      for (const r of results) {
        const change = r.optimal !== r.default ? ` (was ${r.default})` : '';
        console.log(`  ${r.param}: ${r.optimal}${change} -> F1: ${r.f1}`);
      }
    }
  }
}
