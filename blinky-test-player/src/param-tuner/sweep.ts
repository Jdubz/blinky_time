/**
 * Phase 2: Parameter Sweeps
 * Sweeps each parameter independently to find optimal values
 */

import cliProgress from 'cli-progress';
import type { DetectionMode, ParameterMode, SweepResult, SweepPoint, TunerOptions, ParameterDef, TestResult } from './types.js';
import { PARAMETERS, REPRESENTATIVE_PATTERNS } from './types.js';
import { StateManager, IncrementalSweepProgress } from './state.js';
import { TestRunner } from './runner.js';

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
  console.log('\n🔄 Phase 2: Parameter Sweeps');
  console.log('═'.repeat(50));
  console.log('Sweeping each parameter to find optimal values.\n');

  const runner = new TestRunner(options);
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
      : (REPRESENTATIVE_PATTERNS as unknown as string[]);

    for (const param of params) {
      if (stateManager.isSweepComplete(param.name)) {
        const existing = stateManager.getSweepResult(param.name);
        if (existing) {
          console.log(`✓ ${param.name}: Already complete (optimal: ${existing.optimal.value}, F1: ${existing.optimal.avgF1})`);
        }
        continue;
      }

      console.log(`\nSweeping ${param.name} (${param.mode})...`);
      console.log(`   Values: ${param.sweepValues.join(', ')}`);

      // Set to the correct mode (only for detection modes, not subsystem modes like music/rhythm)
      const isDetectionMode = ['drummer', 'bass', 'hfc', 'spectral', 'hybrid'].includes(param.mode);
      if (isDetectionMode) {
        await runner.setMode(param.mode as DetectionMode);
        await runner.resetDefaults(param.mode as DetectionMode);
      }

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
        barCompleteChar: '█',
        barIncompleteChar: '░',
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

      // Reset to default before next parameter
      await runner.setParameter(param.name, param.default);
    }

    // Calculate optimal params per mode
    updateOptimalParams(stateManager);

    stateManager.markSweepPhaseComplete();
    console.log('\n✅ Sweep phase complete.\n');

  } finally {
    await runner.disconnect();
  }
}

function updateOptimalParams(stateManager: StateManager): void {
  const modes: DetectionMode[] = ['drummer', 'spectral', 'hybrid'];

  for (const mode of modes) {
    const modeParams: Record<string, number> = {};
    const params = Object.values(PARAMETERS).filter(p => p.mode === mode);

    for (const param of params) {
      const sweepResult = stateManager.getSweepResult(param.name);
      if (sweepResult) {
        modeParams[param.name] = sweepResult.optimal.value;
      } else {
        modeParams[param.name] = param.default;
      }
    }

    stateManager.setOptimalParams(mode, modeParams);
  }
}

export async function showSweepSummary(stateManager: StateManager): Promise<void> {
  console.log('\n🔄 Sweep Summary');
  console.log('═'.repeat(50));

  const params = Object.values(PARAMETERS);
  const byMode: Record<ParameterMode, Array<{ param: string; default: number; optimal: number; f1: number }>> = {
    drummer: [],
    bass: [],
    hfc: [],
    spectral: [],
    hybrid: [],
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
        console.log(`  ${r.param}: ${r.optimal}${change} → F1: ${r.f1}`);
      }
    }
  }
}
