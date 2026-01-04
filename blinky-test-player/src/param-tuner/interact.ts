/**
 * Phase 3: Parameter Interactions
 * Tests interactions between ensemble parameters
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Tests weight and threshold interactions for the ensemble detector.
 * Legacy hybrid mode testing has been removed.
 */

import cliProgress from 'cli-progress';
import type { InteractionResult, InteractionPoint, TunerOptions, TestResult } from './types.js';
import { REPRESENTATIVE_PATTERNS } from './types.js';
import { StateManager } from './state.js';
import { TestRunner } from './runner.js';

// Interaction test definitions for ensemble
interface InteractionTest {
  name: string;
  params: string[];
  grid: number[][];  // Values for each param
  patterns?: string[];  // Override patterns if needed
}

const INTERACTION_TESTS: InteractionTest[] = [
  {
    name: 'agree-1-2',
    params: ['agree_1', 'agree_2'],
    grid: [
      [0.4, 0.5, 0.6, 0.7],  // agree_1 values
      [0.7, 0.8, 0.85, 0.9],  // agree_2 values
    ],
  },
  {
    name: 'drummer-spectral-weight',
    params: ['drummer_weight', 'spectral_weight'],
    grid: [
      [0.15, 0.2, 0.25, 0.3],  // drummer_weight values
      [0.15, 0.2, 0.25, 0.3],  // spectral_weight values
    ],
  },
  {
    name: 'drummer-thresh-weight',
    params: ['drummer_thresh', 'drummer_weight'],
    grid: [
      [2.0, 2.5, 3.0],  // drummer_thresh values
      [0.15, 0.2, 0.25],  // drummer_weight values
    ],
  },
];

export async function runInteractions(
  options: TunerOptions,
  stateManager: StateManager
): Promise<void> {
  console.log('\n Phase 3: Parameter Interactions');
  console.log('='.repeat(50));
  console.log('Testing interactions between ensemble parameters.\n');

  const runner = new TestRunner(options);
  await runner.connect();

  try {
    for (const test of INTERACTION_TESTS) {
      if (stateManager.isInteractionComplete(test.name)) {
        const existing = stateManager.getInteractionResult(test.name);
        if (existing) {
          console.log(`${test.name}: Already complete (optimal F1: ${existing.optimal.avgF1})`);
        }
        continue;
      }

      console.log(`\nRunning ${test.name} interaction test...`);
      console.log(`   Params: ${test.params.join(', ')}`);

      // Reset to defaults
      await runner.resetDefaults();

      // Generate grid points
      const gridPoints: Array<Record<string, number>> = [];
      generateGrid(test.params, test.grid, 0, {}, gridPoints);

      console.log(`   Grid size: ${gridPoints.length} combinations`);

      const patterns = test.patterns || [...REPRESENTATIVE_PATTERNS];
      const totalTests = gridPoints.length * patterns.length;

      // Check for resume point
      const resumeIndex = stateManager.getInteractionResumeIndex(test.name);
      const partialResults = stateManager.getPartialInteractionResults(test.name);

      if (resumeIndex > 0) {
        console.log(`   Resuming from combination ${resumeIndex + 1}/${gridPoints.length}`);
      }

      // Progress bar
      const progress = new cliProgress.SingleBar({
        format: '   {bar} {percentage}% | {value}/{total} tests | {eta_formatted}',
        barCompleteChar: '\u2588',
        barIncompleteChar: '\u2591',
        hideCursor: true,
      });
      progress.start(totalTests, resumeIndex * patterns.length);

      // Start with partial results if resuming
      const results: InteractionPoint[] = [...partialResults];

      for (let i = resumeIndex; i < gridPoints.length; i++) {
        const params = gridPoints[i];
        stateManager.setInteractionInProgress(test.name, i);

        // Set parameters
        for (const [param, value] of Object.entries(params)) {
          await runner.setParameter(param, value);
        }

        // Run patterns
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
            console.error(`\n   Error on ${pattern}:`, err);
          }
          progress.increment();
        }

        const n = Object.keys(byPattern).length;
        if (n > 0) {
          const point: InteractionPoint = {
            params,
            avgF1: Math.round((totalF1 / n) * 1000) / 1000,
            avgPrecision: Math.round((totalPrecision / n) * 1000) / 1000,
            avgRecall: Math.round((totalRecall / n) * 1000) / 1000,
            byPattern,
          };
          results.push(point);

          // Save incrementally for resume capability
          stateManager.saveInteractionPoint(test.name, point);
        }
      }

      progress.stop();

      // Clear partial results now that we're done
      stateManager.clearPartialInteractionResults(test.name);

      // Find optimal
      if (results.length === 0) {
        console.error(`   No data collected for ${test.name}, skipping`);
        continue;
      }

      let optimal = results[0];
      for (const result of results) {
        if (result.avgF1 > optimal.avgF1) {
          optimal = result;
        }
      }

      const interactionResult: InteractionResult = {
        name: test.name,
        timestamp: new Date().toISOString(),
        grid: results,
        optimal: {
          params: optimal.params,
          avgF1: optimal.avgF1,
        },
      };

      stateManager.saveInteractionResult(test.name, interactionResult);

      console.log(`   Optimal: ${JSON.stringify(optimal.params)} (F1: ${optimal.avgF1})`);

      // Update optimal params based on interaction results
      updateOptimalFromInteraction(stateManager, interactionResult);
    }

    stateManager.markInteractionPhaseComplete();
    console.log('\n Interaction phase complete.\n');

  } finally {
    await runner.disconnect();
  }
}

function generateGrid(
  params: string[],
  grid: number[][],
  depth: number,
  current: Record<string, number>,
  results: Array<Record<string, number>>
): void {
  if (depth === params.length) {
    results.push({ ...current });
    return;
  }

  for (const value of grid[depth]) {
    current[params[depth]] = value;
    generateGrid(params, grid, depth + 1, current, results);
  }
}

export async function showInteractionSummary(stateManager: StateManager): Promise<void> {
  console.log('\n Interaction Summary');
  console.log('='.repeat(50));

  for (const test of INTERACTION_TESTS) {
    const result = stateManager.getInteractionResult(test.name);
    console.log(`\n${test.name}`);
    if (result) {
      console.log(`  Optimal: ${JSON.stringify(result.optimal.params)}`);
      console.log(`  F1: ${result.optimal.avgF1}`);
    } else {
      console.log('  Not yet tested');
    }
  }
}

/**
 * Update optimal parameters based on interaction test results.
 * If an interaction test finds a better combination than individually
 * swept values, update the main optimal parameter set.
 */
function updateOptimalFromInteraction(
  stateManager: StateManager,
  result: InteractionResult
): void {
  const currentOptimal = stateManager.getOptimalParams();

  // Get the current optimal params for comparison
  // Check if the interaction result has a better F1 than we'd expect
  // from the individual parameter sweeps
  if (result.optimal && result.optimal.params) {
    // Update optimal params with the interaction-found values
    for (const [param, value] of Object.entries(result.optimal.params)) {
      const currentValue = currentOptimal[param];
      if (currentValue === undefined || currentValue !== value) {
        console.log(`   Updating ${param}: ${currentValue} -> ${value} (from interaction)`);
        stateManager.setOptimalParam(param, value as number);
      }
    }
  }
}
