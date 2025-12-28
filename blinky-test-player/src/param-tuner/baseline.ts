/**
 * Phase 1: Baseline Testing
 * Establishes baseline performance for each algorithm with default parameters
 */

import cliProgress from 'cli-progress';
import type { DetectionMode, BaselineResult, TunerOptions, TestResult } from './types.js';
import { DETECTION_MODES, PARAMETERS, ALL_PATTERNS } from './types.js';
import { StateManager } from './state.js';
import { TestRunner } from './runner.js';

export async function runBaseline(
  options: TunerOptions,
  stateManager: StateManager
): Promise<void> {
  console.log('\n📊 Phase 1: Baseline Testing');
  console.log('═'.repeat(50));
  console.log('Establishing baseline performance with default parameters.\n');

  const runner = new TestRunner(options);
  await runner.connect();

  try {
    for (const mode of DETECTION_MODES) {
      if (stateManager.isBaselineComplete(mode)) {
        console.log(`✓ ${mode}: Already complete (skipping)`);
        continue;
      }

      console.log(`\nTesting ${mode} algorithm...`);
      stateManager.setBaselineInProgress(mode);

      // Set mode and reset to defaults
      await runner.setMode(mode);
      await runner.resetDefaults(mode);

      // Get default values
      const defaults: Record<string, number> = {};
      for (const param of Object.values(PARAMETERS)) {
        if (param.mode === mode) {
          defaults[param.name] = param.default;
        }
      }

      // Progress bar for patterns
      const progress = new cliProgress.SingleBar({
        format: '   {bar} {percentage}% | {value}/{total} patterns | {eta_formatted}',
        barCompleteChar: '█',
        barIncompleteChar: '░',
        hideCursor: true,
      });

      const patterns = ALL_PATTERNS as unknown as string[];
      const results: Record<string, TestResult> = {};
      let totalF1 = 0;
      let totalPrecision = 0;
      let totalRecall = 0;

      progress.start(patterns.length, 0);

      for (const pattern of patterns) {
        try {
          // Run pattern 3 times for consistency, take average
          let sumF1 = 0;
          let sumPrecision = 0;
          let sumRecall = 0;
          let lastResult: TestResult | null = null;

          for (let run = 0; run < 3; run++) {
            const result = await runner.runPattern(pattern);
            sumF1 += result.f1;
            sumPrecision += result.precision;
            sumRecall += result.recall;
            lastResult = result;
          }

          if (lastResult) {
            results[pattern] = {
              ...lastResult,
              f1: Math.round((sumF1 / 3) * 1000) / 1000,
              precision: Math.round((sumPrecision / 3) * 1000) / 1000,
              recall: Math.round((sumRecall / 3) * 1000) / 1000,
            };
            totalF1 += results[pattern].f1;
            totalPrecision += results[pattern].precision;
            totalRecall += results[pattern].recall;
          }
        } catch (err) {
          console.error(`\n   Error on ${pattern}:`, err);
        }

        progress.increment();
      }

      progress.stop();

      const n = Object.keys(results).length;
      if (n === 0) {
        console.error(`   No patterns completed for ${mode}, skipping`);
        continue;
      }

      const baseline: BaselineResult = {
        algorithm: mode,
        timestamp: new Date().toISOString(),
        defaults,
        patterns: results,
        overall: {
          avgF1: Math.round((totalF1 / n) * 1000) / 1000,
          avgPrecision: Math.round((totalPrecision / n) * 1000) / 1000,
          avgRecall: Math.round((totalRecall / n) * 1000) / 1000,
        },
      };

      stateManager.saveBaselineResult(mode, baseline);

      console.log(`   Avg F1: ${baseline.overall.avgF1} | Precision: ${baseline.overall.avgPrecision} | Recall: ${baseline.overall.avgRecall}`);
    }

    stateManager.markBaselinePhaseComplete();
    console.log('\n✅ Baseline phase complete.\n');

  } finally {
    await runner.disconnect();
  }
}

export async function showBaselineSummary(stateManager: StateManager): Promise<void> {
  console.log('\n📊 Baseline Summary');
  console.log('═'.repeat(50));

  for (const mode of DETECTION_MODES) {
    const result = stateManager.getBaselineResult(mode);
    if (result) {
      console.log(`\n${mode.toUpperCase()}`);
      console.log(`  F1: ${result.overall.avgF1} | Precision: ${result.overall.avgPrecision} | Recall: ${result.overall.avgRecall}`);
    } else {
      console.log(`\n${mode.toUpperCase()}: Not yet tested`);
    }
  }
}
