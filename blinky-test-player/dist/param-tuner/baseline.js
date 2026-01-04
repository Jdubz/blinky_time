/**
 * Phase 1: Baseline Testing
 * Establishes baseline performance for ensemble detection with default parameters
 *
 * ENSEMBLE ARCHITECTURE (December 2025):
 * All 6 detectors run simultaneously with weighted fusion.
 * Legacy per-mode testing has been removed.
 */
import cliProgress from 'cli-progress';
import { PARAMETERS, ALL_PATTERNS } from './types.js';
import { TestRunner } from './runner.js';
export async function runBaseline(options, stateManager) {
    console.log('\n Phase 1: Ensemble Baseline Testing');
    console.log('='.repeat(50));
    console.log('Establishing baseline performance with default parameters.\n');
    const runner = new TestRunner(options);
    await runner.connect();
    try {
        // Check if baseline already complete
        if (stateManager.isBaselineComplete()) {
            console.log('Baseline already complete (skipping)');
            return;
        }
        // Reset to defaults for ensemble
        await runner.resetDefaults();
        // Use specified patterns or default to all
        const patternsToUse = (options.patterns && options.patterns.length > 0)
            ? options.patterns
            : [...ALL_PATTERNS];
        // Get default values for ensemble parameters
        const defaults = {};
        for (const param of Object.values(PARAMETERS)) {
            if (param.mode === 'ensemble') {
                defaults[param.name] = param.default;
            }
        }
        // Load any partial results from previous interrupted run
        const existingProgress = stateManager.getIncrementalBaselineProgress();
        const results = existingProgress?.partialResults || {};
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
        console.log('Testing ensemble detection...');
        stateManager.setBaselineInProgress();
        // Progress bar for patterns
        const progress = new cliProgress.SingleBar({
            format: '   {bar} {percentage}% | {value}/{total} patterns | {eta_formatted}',
            barCompleteChar: '\u2588',
            barIncompleteChar: '\u2591',
            hideCursor: true,
        });
        progress.start(patternsToUse.length, completedPatterns.size);
        // Log resume info if resuming
        if (completedPatterns.size > 0) {
            console.log(`   Resuming from pattern ${completedPatterns.size + 1}/${patternsToUse.length}`);
        }
        for (const pattern of patternsToUse) {
            // Skip already-completed patterns
            if (completedPatterns.has(pattern)) {
                continue;
            }
            try {
                // Run pattern 3 times for consistency, take average
                let sumF1 = 0;
                let sumPrecision = 0;
                let sumRecall = 0;
                let lastResult = null;
                for (let run = 0; run < 3; run++) {
                    const result = await runner.runPattern(pattern);
                    sumF1 += result.f1;
                    sumPrecision += result.precision;
                    sumRecall += result.recall;
                    lastResult = result;
                }
                if (lastResult) {
                    const avgResult = {
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
                    stateManager.appendBaselinePatternResult(pattern, avgResult);
                }
            }
            catch (err) {
                console.error(`\n   Error on ${pattern}:`, err);
            }
            progress.increment();
        }
        progress.stop();
        const n = Object.keys(results).length;
        if (n === 0) {
            console.error('   No patterns completed, skipping');
            return;
        }
        const baseline = {
            timestamp: new Date().toISOString(),
            defaults,
            patterns: results,
            overall: {
                avgF1: Math.round((totalF1 / n) * 1000) / 1000,
                avgPrecision: Math.round((totalPrecision / n) * 1000) / 1000,
                avgRecall: Math.round((totalRecall / n) * 1000) / 1000,
            },
        };
        stateManager.saveBaselineResult(baseline);
        // Clear incremental progress now that baseline is complete
        stateManager.clearIncrementalBaselineProgress();
        console.log(`   Avg F1: ${baseline.overall.avgF1} | Precision: ${baseline.overall.avgPrecision} | Recall: ${baseline.overall.avgRecall}`);
        stateManager.markBaselinePhaseComplete();
        console.log('\n Baseline phase complete.\n');
    }
    finally {
        await runner.disconnect();
    }
}
export async function showBaselineSummary(stateManager) {
    console.log('\n Baseline Summary');
    console.log('='.repeat(50));
    const result = stateManager.getBaselineResult();
    if (result) {
        console.log('\nENSEMBLE');
        console.log(`  F1: ${result.overall.avgF1} | Precision: ${result.overall.avgPrecision} | Recall: ${result.overall.avgRecall}`);
    }
    else {
        console.log('\nENSEMBLE: Not yet tested');
    }
}
