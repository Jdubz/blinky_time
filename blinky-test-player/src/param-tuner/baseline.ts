/**
 * Phase 1: Baseline Testing
 * Establishes baseline performance for each algorithm with default parameters
 */

import cliProgress from 'cli-progress';
import type { DetectionMode, BaselineResult, TunerOptions, TestResult } from './types.js';
import { DETECTION_MODES, PARAMETERS, ALL_PATTERNS } from './types.js';
import { StateManager, IncrementalBaselineProgress } from './state.js';
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
    // Use specified modes or default to all detection modes
    const modesToTest = (options.modes && options.modes.length > 0)
      ? options.modes.filter((m): m is DetectionMode => DETECTION_MODES.includes(m as DetectionMode))
      : DETECTION_MODES;

    // Use specified patterns or default to all
    const patternsToUse = (options.patterns && options.patterns.length > 0)
      ? options.patterns
      : (ALL_PATTERNS as unknown as string[]);

    for (const mode of modesToTest) {
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

      const patterns = patternsToUse;

      // Load any partial results from previous interrupted run
      const existingProgress = stateManager.getIncrementalBaselineProgress(mode);
      const results: Record<string, TestResult> = existingProgress?.partialResults || {};
      const completedPatterns = new Set(existingProgress?.completedPatterns || []);

      // Calculate already-accumulated totals from partial results
      let totalF1 = 0;
      let totalPrecision = 0;
      let totalRecall = 0;
      for (const result of Object.values(results)) {
        totalF1 += result.f1;
        totalPrecision += result.precision;
        totalRecall += result.recall;
      }

      // Progress bar for patterns
      const progress = new cliProgress.SingleBar({
        format: '   {bar} {percentage}% | {value}/{total} patterns | {eta_formatted}',
        barCompleteChar: '█',
        barIncompleteChar: '░',
        hideCursor: true,
      });

      progress.start(patterns.length, completedPatterns.size);

      // Log resume info if resuming
      if (completedPatterns.size > 0) {
        console.log(`   Resuming from pattern ${completedPatterns.size + 1}/${patterns.length}`);
      }

      for (const pattern of patterns) {
        // Skip already-completed patterns
        if (completedPatterns.has(pattern)) {
          continue;
        }

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
            const avgResult: TestResult = {
              ...lastResult,
              f1: Math.round((sumF1 / 3) * 1000) / 1000,
              precision: Math.round((sumPrecision / 3) * 1000) / 1000,
              recall: Math.round((sumRecall / 3) * 1000) / 1000,
            };
            results[pattern] = avgResult;
            totalF1 += avgResult.f1;
            totalPrecision += avgResult.precision;
            totalRecall += avgResult.recall;

            // INCREMENTAL SAVE: Save after each pattern completes (all 3 runs)
            stateManager.appendBaselinePatternResult(mode, pattern, avgResult);
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

      // Clear incremental progress now that baseline for this mode is complete
      stateManager.clearIncrementalBaselineProgress(mode);

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
