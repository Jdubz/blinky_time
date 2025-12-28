/**
 * Phase 3: Parameter Interactions
 * Tests interactions between parameters, especially for Hybrid mode
 */

import cliProgress from 'cli-progress';
import type { InteractionResult, InteractionPoint, TunerOptions, TestResult } from './types.js';
import { REPRESENTATIVE_PATTERNS } from './types.js';
import { StateManager } from './state.js';
import { TestRunner } from './runner.js';

// Interaction test definitions
interface InteractionTest {
  name: string;
  mode: 'drummer' | 'spectral' | 'hybrid';
  params: string[];
  grid: number[][];  // Values for each param
  patterns?: string[];  // Override patterns if needed
}

const INTERACTION_TESTS: InteractionTest[] = [
  {
    name: 'hybrid-weights',
    mode: 'hybrid',
    params: ['hyfluxwt', 'hydrumwt'],
    grid: [
      [0.3, 0.5, 0.7, 0.9],  // hyfluxwt values
      [0.3, 0.5, 0.7, 0.9],  // hydrumwt values
    ],
  },
  {
    name: 'hybrid-boost',
    mode: 'hybrid',
    params: ['hybothboost'],
    grid: [
      [1.0, 1.2, 1.4, 1.6],  // hybothboost values
    ],
  },
  {
    name: 'drummer-thresh-attack',
    mode: 'drummer',
    params: ['hitthresh', 'attackmult'],
    grid: [
      [2.0, 2.5, 3.0, 3.5],  // hitthresh values
      [1.2, 1.3, 1.4, 1.5],  // attackmult values
    ],
  },
  {
    name: 'drummer-cooldown',
    mode: 'drummer',
    params: ['cooldown'],
    grid: [
      [20, 40, 60, 80, 100],  // cooldown values
    ],
    patterns: ['tempo-sweep', 'fast-tempo', 'simultaneous'],  // Fast patterns that stress cooldown
  },
];

function generateGridCombinations(grid: number[][]): number[][] {
  if (grid.length === 0) return [[]];
  if (grid.length === 1) return grid[0].map(v => [v]);

  const result: number[][] = [];
  const restCombinations = generateGridCombinations(grid.slice(1));

  for (const value of grid[0]) {
    for (const rest of restCombinations) {
      result.push([value, ...rest]);
    }
  }

  return result;
}

export async function runInteractions(
  options: TunerOptions,
  stateManager: StateManager
): Promise<void> {
  console.log('\n🔗 Phase 3: Parameter Interactions');
  console.log('═'.repeat(50));
  console.log('Testing parameter interactions and combinations.\n');

  const runner = new TestRunner(options);
  await runner.connect();

  try {
    for (const test of INTERACTION_TESTS) {
      if (stateManager.isInteractionComplete(test.name)) {
        const existing = stateManager.getInteractionResult(test.name);
        if (existing) {
          const optParams = Object.entries(existing.optimal.params)
            .map(([k, v]) => `${k}=${v}`)
            .join(', ');
          console.log(`✓ ${test.name}: Already complete (optimal: ${optParams}, F1: ${existing.optimal.avgF1})`);
        }
        continue;
      }

      console.log(`\nTesting ${test.name} (${test.mode})...`);

      // Set mode
      await runner.setMode(test.mode);
      await runner.resetDefaults(test.mode);

      // For hybrid mode, use optimal base params from sweeps if available
      if (test.mode === 'hybrid') {
        const optimalHybrid = stateManager.getOptimalParams('hybrid');
        if (optimalHybrid) {
          for (const [param, value] of Object.entries(optimalHybrid)) {
            // Skip params we're testing
            if (!test.params.includes(param)) {
              await runner.setParameter(param, value);
            }
          }
        }
      }

      const patterns = test.patterns || (REPRESENTATIVE_PATTERNS as unknown as string[]);
      const combinations = generateGridCombinations(test.grid);
      const points: InteractionPoint[] = [];

      console.log(`   Testing ${combinations.length} combinations × ${patterns.length} patterns`);

      const resumeIndex = stateManager.getInteractionResumeIndex(test.name);

      // Progress bar
      const totalTests = combinations.length * patterns.length;
      const progress = new cliProgress.SingleBar({
        format: '   {bar} {percentage}% | {value}/{total} tests | {eta_formatted}',
        barCompleteChar: '█',
        barIncompleteChar: '░',
        hideCursor: true,
      });
      progress.start(totalTests, resumeIndex * patterns.length);

      for (let i = resumeIndex; i < combinations.length; i++) {
        const combo = combinations[i];
        stateManager.setInteractionInProgress(test.name, i);

        // Set parameter values
        const params: Record<string, number> = {};
        for (let j = 0; j < test.params.length; j++) {
          params[test.params[j]] = combo[j];
          await runner.setParameter(test.params[j], combo[j]);
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
          points.push({
            params,
            avgF1: Math.round((totalF1 / n) * 1000) / 1000,
            avgPrecision: Math.round((totalPrecision / n) * 1000) / 1000,
            avgRecall: Math.round((totalRecall / n) * 1000) / 1000,
            byPattern,
          });
        }
      }

      progress.stop();

      // Find optimal
      if (points.length === 0) {
        console.error(`   No data collected for ${test.name}, skipping`);
        continue;
      }

      let optimalPoint = points[0];
      for (const point of points) {
        if (point.avgF1 > optimalPoint.avgF1) {
          optimalPoint = point;
        }
      }

      const result: InteractionResult = {
        name: test.name,
        timestamp: new Date().toISOString(),
        grid: points,
        optimal: {
          params: optimalPoint.params,
          avgF1: optimalPoint.avgF1,
        },
      };

      stateManager.saveInteractionResult(test.name, result);

      const optParams = Object.entries(optimalPoint.params)
        .map(([k, v]) => `${k}=${v}`)
        .join(', ');
      console.log(`   Optimal: ${optParams} (F1: ${optimalPoint.avgF1})`);

      // Update optimal params based on interaction results
      updateOptimalFromInteraction(stateManager, test.mode, result);
    }

    stateManager.markInteractionPhaseComplete();
    console.log('\n✅ Interaction phase complete.\n');

  } finally {
    await runner.disconnect();
  }
}

function updateOptimalFromInteraction(
  stateManager: StateManager,
  mode: 'drummer' | 'spectral' | 'hybrid',
  result: InteractionResult
): void {
  const current = stateManager.getOptimalParams(mode) || {};
  const updated = { ...current, ...result.optimal.params };
  stateManager.setOptimalParams(mode, updated);
}

export async function showInteractionSummary(stateManager: StateManager): Promise<void> {
  console.log('\n🔗 Interaction Summary');
  console.log('═'.repeat(50));

  for (const test of INTERACTION_TESTS) {
    const result = stateManager.getInteractionResult(test.name);
    if (result) {
      const optParams = Object.entries(result.optimal.params)
        .map(([k, v]) => `${k}=${v}`)
        .join(', ');
      console.log(`\n${test.name} (${test.mode})`);
      console.log(`  Optimal: ${optParams}`);
      console.log(`  F1: ${result.optimal.avgF1}`);
    } else {
      console.log(`\n${test.name}: Not yet tested`);
    }
  }
}
