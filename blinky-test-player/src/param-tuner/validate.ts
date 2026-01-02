/**
 * Phase 4: Validation
 * Validates optimal parameters across ALL patterns
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy per-mode validation has been removed.
 */

import cliProgress from 'cli-progress';
import type { ValidationResult, TunerOptions, TestResult } from './types.js';
import { ALL_PATTERNS, PARAMETERS } from './types.js';
import { StateManager } from './state.js';
import { TestRunner } from './runner.js';

export async function runValidation(
  options: TunerOptions,
  stateManager: StateManager
): Promise<void> {
  console.log('\n Phase 4: Ensemble Validation');
  console.log('='.repeat(50));
  console.log('Validating optimal parameters across all patterns.\n');

  const runner = new TestRunner(options);
  await runner.connect();

  try {
    // Use specified patterns or default to all
    const patterns = (options.patterns && options.patterns.length > 0)
      ? options.patterns
      : (ALL_PATTERNS as unknown as string[]);

    // Check if already validated
    if (stateManager.isValidationComplete()) {
      const existing = stateManager.getValidationResult();
      if (existing) {
        console.log(`Already validated (F1: ${existing.overall.f1}, Delta: ${existing.vsBaseline.f1Delta > 0 ? '+' : ''}${existing.vsBaseline.f1Delta})`);
      }
      return;
    }

    console.log('Validating ensemble detection...');
    stateManager.setValidationInProgress();

    // Get optimal parameters from sweeps
    const optimalParams = stateManager.getOptimalParams();
    if (!optimalParams || Object.keys(optimalParams).length === 0) {
      console.log('   No optimal parameters found, using defaults');
      // Use defaults
      for (const param of Object.values(PARAMETERS)) {
        if (param.mode === 'ensemble') {
          await runner.setParameter(param.name, param.default);
        }
      }
    } else {
      console.log(`   Using optimal params: ${JSON.stringify(optimalParams)}`);
      // Apply optimal parameters
      for (const [param, value] of Object.entries(optimalParams)) {
        await runner.setParameter(param, value);
      }
    }

    // Progress bar
    const progress = new cliProgress.SingleBar({
      format: '   {bar} {percentage}% | {value}/{total} patterns | {eta_formatted}',
      barCompleteChar: '\u2588',
      barIncompleteChar: '\u2591',
      hideCursor: true,
    });

    const results: Record<string, TestResult> = {};
    let totalF1 = 0;
    let totalPrecision = 0;
    let totalRecall = 0;

    progress.start(patterns.length, 0);

    for (const pattern of patterns) {
      try {
        // Run pattern 3 times for consistency
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
      console.error('   No patterns completed, skipping validation');
      return;
    }

    const avgF1 = Math.round((totalF1 / n) * 1000) / 1000;
    const avgPrecision = Math.round((totalPrecision / n) * 1000) / 1000;
    const avgRecall = Math.round((totalRecall / n) * 1000) / 1000;

    // Compare to baseline
    const baseline = stateManager.getBaselineResult();
    const baselineF1 = baseline?.overall.avgF1 ?? 0;
    const f1Delta = Math.round((avgF1 - baselineF1) * 1000) / 1000;

    // Find improved and regressed patterns
    const improved: string[] = [];
    const regressed: string[] = [];

    if (baseline) {
      for (const pattern of Object.keys(results)) {
        const validResult = results[pattern];
        const baseResult = baseline.patterns[pattern];
        if (baseResult) {
          const diff = validResult.f1 - baseResult.f1;
          if (diff > 0.05) {
            improved.push(pattern);
          } else if (diff < -0.05) {
            regressed.push(pattern);
          }
        }
      }
    }

    const validation: ValidationResult = {
      params: optimalParams || {},
      timestamp: new Date().toISOString(),
      patterns: results,
      overall: {
        f1: avgF1,
        precision: avgPrecision,
        recall: avgRecall,
      },
      vsBaseline: {
        f1Delta,
        improved,
        regressed,
      },
    };

    stateManager.saveValidationResult(validation);

    console.log(`   F1: ${avgF1} | Precision: ${avgPrecision} | Recall: ${avgRecall}`);
    console.log(`   vs Baseline: ${f1Delta > 0 ? '+' : ''}${f1Delta}`);
    if (improved.length > 0) {
      console.log(`   Improved: ${improved.join(', ')}`);
    }
    if (regressed.length > 0) {
      console.log(`   Regressed: ${regressed.join(', ')}`);
    }

    stateManager.markValidationPhaseComplete();
    console.log('\n Validation phase complete.\n');

  } finally {
    await runner.disconnect();
  }
}

export async function showValidationSummary(stateManager: StateManager): Promise<void> {
  console.log('\n Validation Summary');
  console.log('='.repeat(50));

  const result = stateManager.getValidationResult();
  const baseline = stateManager.getBaselineResult();

  console.log('\nENSEMBLE');

  if (!result) {
    console.log('  Not yet validated');
    return;
  }

  console.log(`  F1: ${result.overall.f1} | Precision: ${result.overall.precision} | Recall: ${result.overall.recall}`);

  if (baseline) {
    const delta = result.vsBaseline.f1Delta;
    console.log(`  vs Baseline: ${delta > 0 ? '+' : ''}${delta}`);
  }

  console.log('  Optimal params:');
  for (const [param, value] of Object.entries(result.params)) {
    const def = PARAMETERS[param]?.default;
    const change = value !== def ? ` (default: ${def})` : '';
    console.log(`    ${param}: ${value}${change}`);
  }
}
