/**
 * Phase 2: Parameter Sweeps
 * Sweeps each parameter independently to find optimal values
 */

import cliProgress from 'cli-progress';
import type { DetectionMode, SweepResult, SweepPoint, TunerOptions } from './types.js';
import { PARAMETERS, REPRESENTATIVE_PATTERNS } from './types.js';
import { StateManager } from './state.js';
import { TestRunner } from './runner.js';

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
    const params = Object.values(PARAMETERS);
    const patterns = REPRESENTATIVE_PATTERNS as unknown as string[];

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

      // Set to the correct mode
      await runner.setMode(param.mode);
      await runner.resetDefaults(param.mode);

      const sweepPoints: SweepPoint[] = [];
      const resumeIndex = stateManager.getSweepResumeIndex(param.name);

      // Progress bar
      const totalTests = param.sweepValues.length * patterns.length;
      const progress = new cliProgress.SingleBar({
        format: '   {bar} {percentage}% | {value}/{total} tests | {eta_formatted}',
        barCompleteChar: '█',
        barIncompleteChar: '░',
        hideCursor: true,
      });
      progress.start(totalTests, resumeIndex * patterns.length);

      for (let i = resumeIndex; i < param.sweepValues.length; i++) {
        const value = param.sweepValues[i];
        stateManager.setSweepInProgress(param.name, i);

        // Set the parameter value
        await runner.setParameter(param.name, value);

        // Run all representative patterns
        const byPattern: Record<string, import('./types.js').TestResult> = {};
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
          sweepPoints.push({
            value,
            avgF1: Math.round((totalF1 / n) * 1000) / 1000,
            avgPrecision: Math.round((totalPrecision / n) * 1000) / 1000,
            avgRecall: Math.round((totalRecall / n) * 1000) / 1000,
            byPattern,
          });
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
  const byMode: Record<DetectionMode, Array<{ param: string; default: number; optimal: number; f1: number }>> = {
    drummer: [],
    spectral: [],
    hybrid: [],
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
