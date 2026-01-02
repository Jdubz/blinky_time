/**
 * Dynamic Parameter Bounds Extension
 * Tests edge cases to determine if parameter bounds should be extended
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * Tests ensemble parameter limits.
 * Legacy mode-specific testing has been removed.
 */

import type { TunerOptions, TestResult } from './types.js';
import { PARAMETERS } from './types.js';
import { TestRunner } from './runner.js';

interface TestPoint {
  value: number;
  f1: number;
  precision: number;
  recall: number;
}

interface BoundsTestResult {
  param: string;
  direction: 'lower' | 'upper';
  testedValue: number;
  f1: number;
  recommendExtend: boolean;
}

const TEST_PATTERNS = ['strong-beats', 'bass-line', 'synth-stabs', 'fast-tempo'];

/**
 * Test parameter bounds to see if we should extend them
 */
export async function testParameterBounds(
  options: TunerOptions,
  params?: string[]
): Promise<BoundsTestResult[]> {
  console.log('\n Testing Parameter Bounds');
  console.log('='.repeat(50));

  const runner = new TestRunner(options);
  await runner.connect();

  const results: BoundsTestResult[] = [];

  try {
    // Reset to defaults
    await runner.resetDefaults();

    // Get ensemble parameters to test
    const paramsToTest = params
      ? Object.values(PARAMETERS).filter(p => params.includes(p.name) && p.mode === 'ensemble')
      : Object.values(PARAMETERS).filter(p => p.mode === 'ensemble');

    for (const param of paramsToTest) {
      console.log(`\nTesting ${param.name} bounds...`);

      // Test lower bound
      const lowerTest = param.min * 0.8;
      if (lowerTest > 0) {
        await runner.setParameter(param.name, lowerTest);

        let totalF1 = 0;
        let count = 0;
        for (const pattern of TEST_PATTERNS.slice(0, 3)) {
          try {
            const result = await runner.runPattern(pattern);
            totalF1 += result.f1;
            count++;
          } catch {
            // Ignore errors
          }
        }

        const avgF1 = count > 0 ? totalF1 / count : 0;
        const shouldExtend = avgF1 > 0.5; // If it's still functional, consider extending

        results.push({
          param: param.name,
          direction: 'lower',
          testedValue: lowerTest,
          f1: Math.round(avgF1 * 1000) / 1000,
          recommendExtend: shouldExtend,
        });

        console.log(`  Lower (${lowerTest}): F1=${avgF1.toFixed(3)} ${shouldExtend ? '- consider extending' : ''}`);
      }

      // Test upper bound
      const upperTest = param.max * 1.2;
      await runner.setParameter(param.name, upperTest);

      let totalF1 = 0;
      let count = 0;
      for (const pattern of TEST_PATTERNS.slice(0, 3)) {
        try {
          const result = await runner.runPattern(pattern);
          totalF1 += result.f1;
          count++;
        } catch {
          // Ignore errors
        }
      }

      const avgF1 = count > 0 ? totalF1 / count : 0;
      const shouldExtend = avgF1 > 0.5;

      results.push({
        param: param.name,
        direction: 'upper',
        testedValue: upperTest,
        f1: Math.round(avgF1 * 1000) / 1000,
        recommendExtend: shouldExtend,
      });

      console.log(`  Upper (${upperTest}): F1=${avgF1.toFixed(3)} ${shouldExtend ? '- consider extending' : ''}`);

      // Reset parameter
      await runner.setParameter(param.name, param.default);
    }

    // Summary
    console.log('\n Summary:');
    const recommendations = results.filter(r => r.recommendExtend);
    if (recommendations.length === 0) {
      console.log('  No bound extensions recommended.');
    } else {
      console.log('  Consider extending:');
      for (const r of recommendations) {
        console.log(`    ${r.param} ${r.direction}: tested ${r.testedValue}, F1=${r.f1}`);
      }
    }

  } finally {
    await runner.disconnect();
  }

  return results;
}

async function sweepParameter(runner: TestRunner, param: string, values: number[]): Promise<TestPoint[]> {
  const results: TestPoint[] = [];

  for (const value of values) {
    await runner.setParameter(param, value);

    let totalF1 = 0;
    let totalPrecision = 0;
    let totalRecall = 0;
    let count = 0;

    for (const pattern of TEST_PATTERNS) {
      try {
        const result = await runner.runPattern(pattern);
        totalF1 += result.f1;
        totalPrecision += result.precision;
        totalRecall += result.recall;
        count++;
      } catch {
        // Ignore errors
      }
    }

    if (count > 0) {
      results.push({
        value,
        f1: Math.round((totalF1 / count) * 1000) / 1000,
        precision: Math.round((totalPrecision / count) * 1000) / 1000,
        recall: Math.round((totalRecall / count) * 1000) / 1000,
      });

      console.log(`   ${param}=${value}: F1=${(totalF1 / count).toFixed(3)}`);
    }
  }

  return results;
}

function findBest(points: TestPoint[]): TestPoint {
  if (points.length === 0) {
    return { value: 0, f1: 0, precision: 0, recall: 0 };
  }
  return points.reduce((best, point) => point.f1 > best.f1 ? point : best, points[0]);
}

/**
 * Run extended bounds test for specific parameters
 */
export async function runExtendedBoundsTest(options: TunerOptions): Promise<void> {
  console.log('\n Extended Bounds Testing');
  console.log('='.repeat(50));
  console.log('Testing parameters that hit their limits\n');

  const runner = new TestRunner(options);
  await runner.connect();

  try {
    // Reset to defaults first
    await runner.resetDefaults();

    const results: Record<string, TestPoint[]> = {};

    // Test agree_1 (single detector suppression)
    console.log('\n1. Testing agree_1 [0.3, 0.4, 0.5, 0.6, 0.7, 0.8]');
    console.log('-'.repeat(40));
    results.agree_1 = await sweepParameter(runner, 'agree_1', [0.3, 0.4, 0.5, 0.6, 0.7, 0.8]);
    const bestAgree1 = findBest(results.agree_1);
    console.log(`   Best: ${bestAgree1.value} (F1: ${bestAgree1.f1})`);
    await runner.setParameter('agree_1', bestAgree1.value);

    // Test agree_2 (two detector agreement)
    console.log('\n2. Testing agree_2 [0.6, 0.7, 0.8, 0.85, 0.9, 0.95]');
    console.log('-'.repeat(40));
    results.agree_2 = await sweepParameter(runner, 'agree_2', [0.6, 0.7, 0.8, 0.85, 0.9, 0.95]);
    const bestAgree2 = findBest(results.agree_2);
    console.log(`   Best: ${bestAgree2.value} (F1: ${bestAgree2.f1})`);
    await runner.setParameter('agree_2', bestAgree2.value);

    // Test drummer_thresh
    console.log('\n3. Testing drummer_thresh [1.5, 2.0, 2.5, 3.0, 3.5, 4.0]');
    console.log('-'.repeat(40));
    results.drummer_thresh = await sweepParameter(runner, 'drummer_thresh', [1.5, 2.0, 2.5, 3.0, 3.5, 4.0]);
    const bestDrumThresh = findBest(results.drummer_thresh);
    console.log(`   Best: ${bestDrumThresh.value} (F1: ${bestDrumThresh.f1})`);

    // Summary
    console.log('\n Summary');
    console.log('='.repeat(50));
    console.log(`agree_1: ${bestAgree1.value} (F1: ${bestAgree1.f1})`);
    console.log(`agree_2: ${bestAgree2.value} (F1: ${bestAgree2.f1})`);
    console.log(`drummer_thresh: ${bestDrumThresh.value} (F1: ${bestDrumThresh.f1})`);

  } finally {
    await runner.disconnect();
  }
}
